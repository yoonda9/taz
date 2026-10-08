#!/usr/bin/env python3
"""Disposable Windows build VM on Proxmox, behind the `just win*` recipes.

The VM is a linked clone of a Proxmox template that already holds a warm
C:\\taz checkout (see the template's Proxmox description). Code reaches it by
`git send-pack` over SSH, never through a forge, so the VM needs no
credentials.

Usage: win.py COMMAND | RECIPE [ARGS...]
    up               clone the template (if needed), start, wait for SSH
    ssh              interactive shell on the VM
    reset            destroy the VM and clone it again
    down             stop and destroy the VM
    RECIPE [ARGS]    push HEAD to the VM's C:\\taz, then run `just RECIPE ARGS`
                     there (`run RECIPE` if a recipe is named like a command)

Settings come from the environment, falling back to the repo's .env:
    PVE_URL, PVE_TOKEN_ID, PVE_TOKEN   Proxmox API host and token (required)
    TAZ_WIN_NODE       Proxmox node that holds the templates (required)
    TAZ_WIN_TEMPLATE   template VMID (9101)       TAZ_WIN_VMID     clone VMID (9200)
    TAZ_WIN_POOL       pool (taz)
    TAZ_WIN_MEMORY     clone RAM in MB (8192)     TAZ_WIN_KEY      SSH private key
    WIN_USER           Windows user (user)
"""

from __future__ import annotations

import json
import os
import shlex
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
REMOTE_REPO = "C:/taz"
# Every clone shares the template's SSH host key, so it is pinned once under
# this alias instead of once per DHCP address.
HOST_KEY_ALIAS = "win-build"
KNOWN_HOSTS = Path.home() / ".ssh" / "known_hosts_taz_win"
# The same environment as CI's Windows job.
REMOTE_ENV = {
    "CI": "1",
    "UV_LOCKED": "1",
    "TAZ_DAEMON": "daemon/build/windows-debug/tazd.exe",
}
WAIT_SECONDS = 600
# `up` tags the clones it makes; `down` destroys nothing without this tag.
CLONE_TAG = "win-clone"


@dataclass(frozen=True)
class Config:
    api: str
    auth: str
    node: str
    pool: str
    template: int
    vmid: int
    memory: int
    key: Path
    user: str
    known_hosts: Path = KNOWN_HOSTS

    @property
    def vm_path(self) -> str:
        return f"/nodes/{self.node}/qemu/{self.vmid}"

    @property
    def hostname(self) -> str:
        return f"win-{self.vmid}"


def setting(name: str, default: str | None = None) -> str:
    """A setting from the environment, else the repo's .env, else the default."""
    dotenv: dict[str, str] = {}
    env_file = ROOT / ".env"
    if env_file.is_file():
        for line in env_file.read_text().splitlines():
            key, sep, raw = line.strip().partition("=")
            if sep and not key.startswith("#"):
                dotenv[key.strip()] = raw.strip().strip("'\"")
    value = os.environ.get(name) or dotenv.get(name) or default
    if value is None:
        sys.exit(f"error: {name} is not set (environment or .env)")
    return value


def load_config() -> Config:
    get = setting
    return Config(
        api=f"https://{get('PVE_URL')}:8006/api2/json",
        auth=f"PVEAPIToken={get('PVE_TOKEN_ID')}={get('PVE_TOKEN')}",
        node=get("TAZ_WIN_NODE"),
        pool=get("TAZ_WIN_POOL", "taz"),
        template=int(get("TAZ_WIN_TEMPLATE", "9101")),
        vmid=int(get("TAZ_WIN_VMID", "9200")),
        memory=int(get("TAZ_WIN_MEMORY", "8192")),
        key=Path(get("TAZ_WIN_KEY", str(Path.home() / ".ssh" / "taz_win_ed25519"))),
        user=get("WIN_USER", "user"),
    )


# ---------------------------------------------------------------------------
# Proxmox API
# ---------------------------------------------------------------------------
# The lab Proxmox host serves a self-signed certificate.
TLS = ssl.create_default_context()
TLS.check_hostname = False
TLS.verify_mode = ssl.CERT_NONE


def api(
    cfg: Config,
    method: str,
    path: str,
    data: dict[str, str | int] | None = None,
    *,
    body: bytes | None = None,
    content_type: str | None = None,
) -> Any:
    headers = {"Authorization": cfg.auth}
    if data:
        body = urllib.parse.urlencode(data).encode()
    elif content_type:
        headers["Content-Type"] = content_type
    request = urllib.request.Request(  # noqa: S310 - https URL built from PVE_URL
        cfg.api + path, data=body, method=method, headers=headers
    )
    try:
        with urllib.request.urlopen(request, context=TLS, timeout=300) as response:  # noqa: S310
            return json.load(response)["data"]
    except urllib.error.HTTPError as err:
        raise ApiError(f"{method} {path}: {err.code} {err.reason}") from None


class ApiError(Exception):
    pass


def wait_task(cfg: Config, upid: str) -> None:
    path = f"/nodes/{cfg.node}/tasks/{urllib.parse.quote(upid, safe='')}/status"
    status = wait_for(lambda: _finished(api(cfg, "GET", path)), f"task {upid}")
    if status != "OK":
        sys.exit(f"error: Proxmox task failed: {status}")


def _finished(task: dict[str, Any]) -> str | None:
    return str(task.get("exitstatus")) if task.get("status") == "stopped" else None


def wait_for[T](
    probe: Callable[[], T | None], what: str, timeout: int = WAIT_SECONDS
) -> T:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = probe()
        except ApiError:
            value = None
        if value is not None:
            return value
        time.sleep(3)
    sys.exit(f"error: timed out after {timeout}s waiting for {what}")


def vm_status(cfg: Config) -> str | None:
    for vm in api(cfg, "GET", "/cluster/resources?type=vm"):
        if vm.get("vmid") == cfg.vmid:
            return str(vm.get("status"))
    return None


def guest_ip(cfg: Config) -> str | None:
    result = api(cfg, "GET", f"{cfg.vm_path}/agent/network-get-interfaces")
    for iface in result.get("result", []):
        for addr in iface.get("ip-addresses", []):
            ip = str(addr.get("ip-address", ""))
            if addr.get("ip-address-type") == "ipv4" and not ip.startswith(
                ("127.", "169.254.")
            ):
                return ip
    return None


# ---------------------------------------------------------------------------
# SSH
# ---------------------------------------------------------------------------
def ssh_options(cfg: Config) -> list[str]:
    return [
        "-i", str(cfg.key),
        "-o", "IdentitiesOnly=yes",
        "-o", "ConnectTimeout=10",
        "-o", f"HostKeyAlias={HOST_KEY_ALIAS}",
        "-o", f"UserKnownHostsFile={cfg.known_hosts}",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "HashKnownHosts=no",
        # `run` goes silent for minutes at a time (e.g. linking taz_tests.exe
        # while Defender real-time-scans the fresh .exe/.pdb); without this,
        # an idle network device between here and the VM drops the
        # connection and the build is orphaned with no error.
        "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=4",
    ]  # fmt: skip


def ssh(cfg: Config, ip: str, command: str, *, capture: bool = False) -> str | None:
    result = subprocess.run(
        ["ssh", *ssh_options(cfg), "-o", "BatchMode=yes", f"{cfg.user}@{ip}", command],
        capture_output=capture,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return None
    return result.stdout.strip() if capture else ""


def remote_hostname(cfg: Config) -> str | None:
    ip = guest_ip(cfg)
    return ssh(cfg, ip, "hostname", capture=True) if ip else None


def ps_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------
def log(t0: float, message: str) -> None:
    print(f"[win +{time.monotonic() - t0:.0f}s] {message}", flush=True)


def cmd_up(cfg: Config) -> str:
    t0 = time.monotonic()
    status = vm_status(cfg)
    if status is None:
        upid = api(
            cfg,
            "POST",
            f"/nodes/{cfg.node}/qemu/{cfg.template}/clone",
            {
                "newid": cfg.vmid,
                "name": f"taz-win-{cfg.vmid}",
                "full": 0,
                "pool": cfg.pool,
            },
        )
        wait_task(cfg, upid)
        api(
            cfg,
            "PUT",
            f"{cfg.vm_path}/config",
            {
                "memory": cfg.memory,
                "balloon": min(cfg.memory, 4096),
                "tags": f"build;taz;windows;{CLONE_TAG}",
            },
        )
        log(t0, f"cloned template {cfg.template} to VM {cfg.vmid}")
    if status != "running":
        wait_task(cfg, api(cfg, "POST", f"{cfg.vm_path}/status/start"))
        log(t0, f"started VM {cfg.vmid}")

    ip = wait_for(lambda: guest_ip(cfg), "the guest agent to report an IP")
    name = wait_for(lambda: remote_hostname(cfg), f"SSH on {ip}")
    if name.lower() != cfg.hostname:
        # Clones share the template's hostname; give each its own. Renaming
        # needs a reboot, and the SSH session drops with it.
        ssh(cfg, ip, f"Rename-Computer -NewName {cfg.hostname} -Force -Restart")
        log(t0, f"renamed {name} to {cfg.hostname}, rebooting")

        def renamed() -> bool | None:
            current = remote_hostname(cfg)
            return True if current and current.lower() == cfg.hostname else None

        wait_for(renamed, f"{cfg.hostname} to come back")
        ip = wait_for(lambda: guest_ip(cfg), "the guest agent to report an IP")
    log(t0, f"VM {cfg.vmid} ({cfg.hostname}) ready: ssh {cfg.user}@{ip}")
    return ip


def push_head(cfg: Config, ip: str) -> int:
    """Push HEAD to the VM's checked-out `main`, which updates C:\\taz in place."""
    if subprocess.run(
        ["git", "diff", "--quiet", "HEAD"], cwd=ROOT, check=False
    ).returncode:
        print(
            "warning: uncommitted changes are not pushed; the VM gets HEAD",
            file=sys.stderr,
            flush=True,
        )
    # A previous run can leave tracked files modified, and updateInstead
    # refuses to update a dirty tree. Untracked build trees stay warm.
    ssh(cfg, ip, f"git -C {REMOTE_REPO} reset --hard -q")
    env = dict(os.environ, GIT_SSH_COMMAND=shlex.join(["ssh", *ssh_options(cfg)]))
    return subprocess.run(
        [
            "git",
            "send-pack",
            "--receive-pack=git receive-pack",
            "--force",
            f"{cfg.user}@{ip}:{REMOTE_REPO}",
            "HEAD:refs/heads/main",
        ],
        cwd=ROOT,
        env=env,
        check=False,
    ).returncode


def cmd_run(cfg: Config, args: Sequence[str]) -> int:
    ip = cmd_up(cfg)
    if code := push_head(cfg, ip):
        return code
    exports = "; ".join(f"$env:{k}={ps_quote(v)}" for k, v in REMOTE_ENV.items())
    recipe = " ".join(ps_quote(a) for a in args)
    command = (
        f"Set-Location {REMOTE_REPO}; {exports}; "
        "git submodule update --init --recursive -q; "
        f"just {recipe}; exit $LASTEXITCODE"
    )
    return subprocess.run(
        ["ssh", *ssh_options(cfg), "-o", "BatchMode=yes", f"{cfg.user}@{ip}", command],
        check=False,
    ).returncode


def cmd_ssh(cfg: Config) -> int:
    ip = cmd_up(cfg)
    return subprocess.run(
        ["ssh", *ssh_options(cfg), "-t", f"{cfg.user}@{ip}"], check=False
    ).returncode


def is_disposable(vm_config: dict[str, Any]) -> bool:
    """True for a VM that `up` created: tagged CLONE_TAG and not a template."""
    tags = str(vm_config.get("tags", "")).split(";")
    return CLONE_TAG in tags and not vm_config.get("template")


def cmd_down(cfg: Config) -> None:
    status = vm_status(cfg)
    if status is None:
        print(f"VM {cfg.vmid} does not exist")
        return
    # Only ever destroy a clone that `up` made, never a template or a VM such
    # as the build box. (lvmthin volume names do not record a clone's parent.)
    if not is_disposable(api(cfg, "GET", f"{cfg.vm_path}/config")):
        sys.exit(
            f"error: VM {cfg.vmid} has no {CLONE_TAG!r} tag, so `up` did not "
            "create it; refusing to destroy it"
        )
    t0 = time.monotonic()
    if status == "running":
        wait_task(cfg, api(cfg, "POST", f"{cfg.vm_path}/status/stop"))
    wait_task(
        cfg,
        api(cfg, "DELETE", f"{cfg.vm_path}?purge=1&destroy-unreferenced-disks=1"),
    )
    log(t0, f"destroyed VM {cfg.vmid}")


# ---------------------------------------------------------------------------
def main(argv: Sequence[str] | None = None) -> None:
    # Not argparse: anything that is not a local command is a recipe for the
    # VM, so `just win down` and `just win test` both read naturally.
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in {"-h", "--help"}:
        print(__doc__)
        return

    cfg = load_config()
    match args[0]:
        case "up":
            cmd_up(cfg)
        case "ssh":
            sys.exit(cmd_ssh(cfg))
        case "reset":
            cmd_down(cfg)
            cmd_up(cfg)
        case "down":
            cmd_down(cfg)
        case "run":
            sys.exit(cmd_run(cfg, args[1:]))
        case _:
            sys.exit(cmd_run(cfg, args))


if __name__ == "__main__":
    main()
