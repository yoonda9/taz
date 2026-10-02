#!/usr/bin/env python3
"""Rebuild the Windows build templates from ISO, behind `just win-rebuild`.

Usage: winbuild.py all | layer
    all      install Windows from ISO into a staging VM, provision the
             toolchain, replace the base template with it, then rebuild the
             TAZ layer on top (about 1.5 hours)
    layer    rebuild only the TAZ layer on the current base (about 15 minutes)

Both finish by running `just test` on a clone of the new layer. `all`
destroys the old templates only once the new base has passed its checks.
Template descriptions get a password placeholder; fill it in by hand.

Settings (environment, else .env), on top of tools/win.py's:
    WIN_PASSWORD        password for WIN_USER and Administrator (required)
    TAZ_WIN_BASE        base template VMID (9100)
    TAZ_WIN_STAGING     VMID used while installing (9150)
    TAZ_WIN_ISO         Windows install ISO volume (the Server 2025 eval ISO)
    TAZ_WIN_VIRTIO_ISO  VirtIO drivers ISO volume (local:iso/virtio-win.iso)
"""

from __future__ import annotations

import dataclasses
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
from collections.abc import Sequence
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import win

ROOT = win.ROOT
IMAGE_DIR = ROOT / "tools" / "winimage"
BASE_NAME = "win-build-base"
LAYER_NAME = "taz-win-tpl"
HOSTNAME = "win-build"
ISO_STORAGE = "local"
DISK_STORAGE = "local-lvm"
BRIDGE = "vmbr0"
# A VM this script is still building; it destroys only these and its own
# templates (by name), never anything else.
STAGING_TAG = "win-staging"
INSTALL_SECONDS = 3600


@dataclasses.dataclass(frozen=True)
class Build:
    cfg: win.Config
    base: int
    staging: int
    iso: str
    virtio_iso: str
    password: str
    t0: float = dataclasses.field(default_factory=time.monotonic)

    @property
    def layer(self) -> int:
        return self.cfg.template

    def at(self, vmid: int, known_hosts: Path | None = None) -> win.Config:
        return dataclasses.replace(
            self.cfg, vmid=vmid, known_hosts=known_hosts or self.cfg.known_hosts
        )

    def log(self, message: str) -> None:
        elapsed = int(time.monotonic() - self.t0)
        print(f"[rebuild {elapsed // 60:02d}:{elapsed % 60:02d}] {message}", flush=True)


def load_build() -> Build:
    get = win.setting
    return Build(
        cfg=win.load_config(),
        base=int(get("TAZ_WIN_BASE", "9100")),
        staging=int(get("TAZ_WIN_STAGING", "9150")),
        iso=get(
            "TAZ_WIN_ISO",
            "local:iso/26100.32230.260111-0550.lt_release_svc_refresh"
            "_SERVER_EVAL_x64FRE_en-us.iso",
        ),
        virtio_iso=get("TAZ_WIN_VIRTIO_ISO", "local:iso/virtio-win.iso"),
        password=get("WIN_PASSWORD"),
    )


def render(template: str, values: dict[str, str]) -> str:
    text = (IMAGE_DIR / template).read_text()
    for key, value in values.items():
        text = text.replace(f"@@{key}@@", value)
    if "@@" in text:
        sys.exit(f"error: {template} has a placeholder this script does not fill")
    return text


def ci_pins() -> list[str]:
    """provision.ps1 arguments: the tool versions CI's Windows job pins."""
    text = (ROOT / ".github" / "workflows" / "ci.yml").read_text()

    def pin(name: str) -> str:
        match = re.search(rf"^\s*{name}:\s*\"?([^\s\"]+)", text, re.MULTILINE)
        if not match:
            sys.exit(f"error: {name} not found in ci.yml")
        return match.group(1)

    return [
        f"-MiseVersion {pin('MISE_VERSION')}",
        f"-CppcheckVersion {pin('CPPCHECK_VERSION')}",
        f"-CppcheckSha256 {pin('CPPCHECK_SHA256')}",
    ]


# ---------------------------------------------------------------------------
# Proxmox
# ---------------------------------------------------------------------------
def wait(b: Build, upid: str) -> None:
    win.wait_task(b.cfg, upid)


def vm_configs(b: Build) -> dict[int, dict[str, Any]]:
    return {
        int(vm["vmid"]): vm
        for vm in win.api(b.cfg, "GET", "/cluster/resources?type=vm")
    }


def destroy(b: Build, vmid: int) -> None:
    cfg = b.at(vmid)
    if win.vm_status(cfg) == "running":
        wait(b, win.api(cfg, "POST", f"{cfg.vm_path}/status/stop"))
    wait(
        b, win.api(cfg, "DELETE", f"{cfg.vm_path}?purge=1&destroy-unreferenced-disks=1")
    )
    b.log(f"destroyed VM {vmid}")


def destroy_ours(b: Build, vmid: int, template_name: str) -> None:
    """Destroy VMID if it is this script's template or an unfinished build."""
    vm = vm_configs(b).get(vmid)
    if vm is None:
        return
    tags = str(vm.get("tags", "")).split(";")
    ours = (vm.get("template") and vm.get("name") == template_name) or (
        STAGING_TAG in tags and not vm.get("template")
    )
    if not ours:
        sys.exit(
            f"error: VM {vmid} ({vm.get('name')}) is not a template or build this "
            "script made; refusing to destroy it"
        )
    destroy(b, vmid)


def destroy_clones(b: Build) -> None:
    for vmid, vm in vm_configs(b).items():
        if win.is_disposable(vm):
            destroy(b, vmid)


def clone(b: Build, source: int, vmid: int, name: str, *, full: bool) -> None:
    data: dict[str, str | int] = {
        "newid": vmid,
        "name": name,
        "full": int(full),
        "pool": b.cfg.pool,
    }
    if full:
        data["storage"] = DISK_STORAGE
    wait(b, win.api(b.cfg, "POST", f"/nodes/{b.cfg.node}/qemu/{source}/clone", data))
    b.log(f"{'full' if full else 'linked'} clone of {source} -> VM {vmid} ({name})")


def start(b: Build, cfg: win.Config) -> None:
    wait(b, win.api(cfg, "POST", f"{cfg.vm_path}/status/start"))


def shutdown(b: Build, cfg: win.Config) -> None:
    wait(b, win.api(cfg, "POST", f"{cfg.vm_path}/status/shutdown", {"timeout": 600}))
    win.wait_for(lambda: True if win.vm_status(cfg) == "stopped" else None, "shutdown")


def make_template(b: Build, cfg: win.Config, description: str, tags: str) -> None:
    win.api(
        cfg, "PUT", f"{cfg.vm_path}/config", {"description": description, "tags": tags}
    )
    wait(b, win.api(cfg, "POST", f"{cfg.vm_path}/template"))
    b.log(f"VM {cfg.vmid} is now a template")


def upload_iso(b: Build, path: Path) -> str:
    boundary = secrets.token_hex(16)
    head = (
        f'--{boundary}\r\nContent-Disposition: form-data; name="content"\r\n\r\niso\r\n'
        f"--{boundary}\r\nContent-Disposition: form-data; "
        f'name="filename"; filename="{path.name}"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n"
    ).encode()
    body = head + path.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
    wait(
        b,
        win.api(
            b.cfg,
            "POST",
            f"/nodes/{b.cfg.node}/storage/{ISO_STORAGE}/upload",
            body=body,
            content_type=f"multipart/form-data; boundary={boundary}",
        ),
    )
    return f"{ISO_STORAGE}:iso/{path.name}"


# ---------------------------------------------------------------------------
# SSH
# ---------------------------------------------------------------------------
def ready_ip(cfg: win.Config, check: str = "hostname") -> str | None:
    """The VM's IP once the guest agent reports one and `check` succeeds."""
    ip = win.guest_ip(cfg)
    if ip and win.ssh(cfg, ip, check, capture=True):
        return ip
    return None


def run_script(cfg: win.Config, ip: str, script: Path, args: Sequence[str] = ()) -> Any:
    """Copy a .ps1 to the VM, stream its output, and return its FACTS json."""
    target = f"{cfg.user}@{ip}"
    subprocess.run(
        ["scp", "-q", *win.ssh_options(cfg), str(script), f"{target}:{script.name}"],
        check=True,
    )
    command = (
        "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass "
        f"-File $HOME\\{script.name} {' '.join(args)}"
    )
    facts: Any = None
    with subprocess.Popen(
        ["ssh", *win.ssh_options(cfg), "-o", "ServerAliveInterval=30", target, command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    ) as proc:
        for line in proc.stdout or []:
            line = line.rstrip()
            if line.startswith("FACTS "):
                facts = json.loads(line[len("FACTS ") :])
            elif line and "RemoteException" not in line:
                print(f"    | {line}", flush=True)
    if proc.returncode or facts is None:
        sys.exit(f"error: {script.name} failed on {ip} (exit {proc.returncode})")
    win.ssh(cfg, ip, f"Remove-Item $HOME\\{script.name} -Force")
    return facts


def set_password(b: Build, cfg: win.Config, ip: str) -> None:
    command = (
        f"foreach ($n in {win.ps_quote(cfg.user)}, 'Administrator') "
        f"{{ net.exe user $n {win.ps_quote(b.password)} | Out-Null; "
        "if ($LASTEXITCODE) { exit 1 } }"
    )
    if win.ssh(cfg, ip, command) is None:
        sys.exit("error: setting the Windows password failed")
    if shutil.which("sshpass"):
        login = subprocess.run(
            [
                "sshpass", "-e", "ssh",
                "-o", "PubkeyAuthentication=no",
                "-o", "PreferredAuthentications=password,keyboard-interactive",
                *win.ssh_options(cfg),
                f"{cfg.user}@{ip}", "whoami",
            ],
            env=dict(os.environ, SSHPASS=b.password),
            capture_output=True,
            text=True,
            check=False,
        )  # fmt: skip
        if login.returncode:
            sys.exit("error: password login fails after setting the password")
    b.log(f"password set for {cfg.user} and Administrator; password login works")


def finish_disk(cfg: win.Config, ip: str) -> None:
    """Drop temp files and TRIM, so the template's thin disk stays small."""
    win.ssh(
        cfg,
        ip,
        "Get-ChildItem $env:TEMP -Force | Remove-Item -Recurse -Force "
        "-ErrorAction SilentlyContinue; Optimize-Volume -DriveLetter C -ReTrim",
    )


def eval_expiry(cfg: win.Config, ip: str) -> str:
    out = win.ssh(
        cfg, ip, "cscript //nologo C:\\Windows\\System32\\slmgr.vbs /dlv", capture=True
    )
    match = re.search(r"Timebased activation expiration:\s*(\d+)\s*minute", out or "")
    if not match:
        return "unknown (run `slmgr /dlv`)"
    expiry = datetime.now(UTC) + timedelta(minutes=int(match.group(1)))
    return expiry.strftime("%Y-%m-%d %H:%M UTC")


def host_keys(ip: str, known_hosts: Path) -> list[str]:
    """All of the VM's host keys under the alias, checked against the key the
    first SSH session accepted (the build trusts its own new VM once)."""
    accepted = {
        line.split()[1:3][1]
        for line in known_hosts.read_text().splitlines()
        if line.startswith(f"{win.HOST_KEY_ALIAS} ")
    }
    scanned = subprocess.run(
        ["ssh-keyscan", "-T", "10", ip], capture_output=True, text=True, check=True
    ).stdout.splitlines()
    lines = sorted(
        f"{win.HOST_KEY_ALIAS} {' '.join(line.split()[1:3])}"
        for line in scanned
        if line and not line.startswith("#")
    )
    if not accepted or not accepted & {line.split()[2] for line in lines}:
        sys.exit("error: ssh-keyscan returned keys the SSH session did not see")
    return lines


def key_values(b: Build, known_hosts: Path) -> dict[str, str]:
    pub = Path(f"{b.cfg.key}.pub")
    fingerprints = subprocess.run(
        ["ssh-keygen", "-lf", str(known_hosts)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    ed25519 = next(
        line
        for line in known_hosts.read_text().splitlines()
        if line.split()[1] == "ssh-ed25519"
    )
    return {
        "PUBKEY": pub.read_text().strip(),
        "PUBKEY_FP": subprocess.run(
            ["ssh-keygen", "-lf", str(pub)], capture_output=True, text=True, check=True
        ).stdout.split()[1],
        "HOSTKEY_FPS": "\n".join(
            f"  - {f.split()[-1].strip('()')} `{f.split()[1]}`" for f in fingerprints
        ),
        "HOSTKEY_LINE": ed25519,
    }


def common_values(b: Build) -> dict[str, str]:
    return {
        "BASE": str(b.base),
        "BASE_NAME": BASE_NAME,
        "LAYER": str(b.layer),
        "LAYER_NAME": LAYER_NAME,
        "HOSTNAME": HOSTNAME,
        "USER": b.cfg.user,
        "NODE": b.cfg.node,
        "POOL": b.cfg.pool,
        "PVE_HOST": win.setting("PVE_URL"),
        "BUILD_DATE": datetime.now(UTC).strftime("%Y-%m-%d"),
    }


# ---------------------------------------------------------------------------
# steps
# ---------------------------------------------------------------------------
def build_base(b: Build) -> None:
    for tool in ("genisoimage", "ssh-keyscan"):
        if not shutil.which(tool):
            sys.exit(f"error: {tool} is not installed")
    isos = {
        v["volid"]
        for v in win.api(
            b.cfg,
            "GET",
            f"/nodes/{b.cfg.node}/storage/{ISO_STORAGE}/content?content=iso",
        )
    }
    for iso in (b.iso, b.virtio_iso):
        if iso not in isos:
            sys.exit(f"error: {iso} is not on storage {ISO_STORAGE}")
    pins = ci_pins()
    destroy_ours(b, b.staging, "")

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        media = work / "media"
        media.mkdir()
        # The install password only has to last until SSH works; the build
        # then sets WIN_PASSWORD. The ISO stays on storage (the token cannot
        # delete ISOs), so it must not hold the real one.
        values = {
            "USER": b.cfg.user,
            "HOSTNAME": HOSTNAME,
            "INSTALL_PASSWORD": secrets.token_urlsafe(18) + "aA1!",
            "PUBKEY": Path(f"{b.cfg.key}.pub").read_text().strip(),
        }
        (media / "autounattend.xml").write_text(render("autounattend.xml.in", values))
        (media / "win-setup.ps1").write_text(render("win-setup.ps1.in", values))
        stamp = datetime.now(UTC).strftime("%Y%m%d-%H%M%S")
        iso_path = work / f"win-build-unattend-{stamp}.iso"
        genisoimage = ["genisoimage", "-quiet", "-J", "-r", "-V", "UNATTEND"]
        subprocess.run([*genisoimage, "-o", str(iso_path), str(media)], check=True)
        answer = upload_iso(b, iso_path)
        b.log(f"uploaded {answer}")

        wait(
            b,
            win.api(
                b.cfg,
                "POST",
                f"/nodes/{b.cfg.node}/qemu",
                {
                    "vmid": b.staging,
                    "name": "win-build-staging",
                    "pool": b.cfg.pool,
                    "ostype": "win11",
                    "machine": "q35",
                    "bios": "seabios",
                    "cpu": "host",
                    "cores": 4,
                    "sockets": 1,
                    "memory": 8192,
                    "balloon": 4096,
                    "agent": "enabled=1",
                    "scsihw": "virtio-scsi-single",
                    # SATA on purpose: VirtIO SCSI broke the unattended install.
                    "sata1": f"{DISK_STORAGE}:120,discard=on,ssd=1",
                    "net0": f"virtio,bridge={BRIDGE}",
                    "ide2": f"{b.iso},media=cdrom",
                    "ide0": f"{b.virtio_iso},media=cdrom",
                    "sata0": f"{answer},media=cdrom",
                    # The empty disk falls through to the installer; once
                    # Windows is on it, the CD's "press any key" never shows.
                    "boot": "order=sata1;ide2",
                },
            ),
        )
        staging_known = work / "known_hosts"
        cfg = b.at(b.staging, staging_known)
        # Tags are permission-checked on the VM itself, which only joins the
        # pool (and its grants) once it exists, so they cannot go in the create.
        win.api(cfg, "PUT", f"{cfg.vm_path}/config", {"tags": STAGING_TAG})
        start(b, cfg)
        b.log(f"VM {b.staging} installing Windows from {b.iso.split('/')[-1]}")

        ip = win.wait_for(
            lambda: ready_ip(cfg, "Test-Path C:/Windows/Temp/win-setup.done"),
            "Windows setup, the guest agent and SSH",
            timeout=INSTALL_SECONDS,
        )
        win.wait_for(
            lambda: (
                True
                if win.ssh(
                    cfg, ip, "Test-Path C:/Windows/Temp/win-setup.done", capture=True
                )
                == "True"
                else None
            ),
            "the first-logon setup to finish",
        )
        b.log(f"Windows is up at {ip}; SSH works")
        set_password(b, cfg, ip)

        b.log("provisioning the toolchain (about 30 minutes)")
        facts = run_script(cfg, ip, IMAGE_DIR / "provision.ps1", pins)
        b.log(f"toolchain: {facts['vs']}, MSVC {facts['msvc']}")
        keys = host_keys(ip, staging_known)
        staging_known.write_text("\n".join(keys) + "\n")
        expiry = (
            datetime.now(UTC) + timedelta(minutes=int(facts["eval_minutes"]))
        ).strftime("%Y-%m-%d %H:%M UTC")
        finish_disk(cfg, ip)
        shutdown(b, cfg)
        empty = "none,media=cdrom"
        win.api(
            cfg,
            "PUT",
            f"{cfg.vm_path}/config",
            {"ide0": empty, "ide2": empty, "sata0": empty, "boot": "order=sata1"},
        )
        b.log(f"VM {b.staging} provisioned and shut down")

        # The new base passed its checks: swap it in. Old clones and the old
        # layer sit on the old base, so they go first.
        destroy_clones(b)
        destroy_ours(b, b.layer, LAYER_NAME)
        destroy_ours(b, b.base, BASE_NAME)
        clone(b, b.staging, b.base, BASE_NAME, full=True)
        description = render(
            "base.md.in",
            common_values(b)
            | key_values(b, staging_known)
            | {
                "ISO": b.iso,
                "OS": facts["os"],
                "EVAL_EXPIRY": expiry,
                "VS": facts["vs"],
                "VS_PATH": facts["vs_path"],
                "MSVC": facts["msvc"],
                "SDK": str(facts["sdk"]),
                "GIT": facts["git"],
                "MISE": facts["mise"],
                "CPPCHECK": facts["cppcheck"],
            },
        )
        make_template(b, b.at(b.base), description, "base;build;template;windows")
        destroy(b, b.staging)

        # Every clone of the new base has these host keys; pin them for
        # tools/win.py and the layer build.
        win.KNOWN_HOSTS.parent.mkdir(parents=True, exist_ok=True)
        win.KNOWN_HOSTS.write_text(staging_known.read_text())
        win.KNOWN_HOSTS.chmod(0o600)
        b.log(f"pinned the new host keys in {win.KNOWN_HOSTS}")


def build_layer(b: Build) -> None:
    destroy_clones(b)
    destroy_ours(b, b.layer, LAYER_NAME)
    clone(b, b.base, b.layer, LAYER_NAME, full=False)
    cfg = b.at(b.layer)
    win.api(cfg, "PUT", f"{cfg.vm_path}/config", {"tags": STAGING_TAG})
    start(b, cfg)
    ip = win.wait_for(lambda: ready_ip(cfg), "the layer VM to answer over SSH")
    if win.ssh(
        cfg,
        ip,
        f"git init -q -b main {win.REMOTE_REPO}; "
        f"git -C {win.REMOTE_REPO} config receive.denyCurrentBranch updateInstead; "
        "Add-MpPreference -ExclusionPath C:\\taz",
    ) is None or win.push_head(cfg, ip):
        sys.exit("error: could not set up C:\\taz on the layer VM")
    b.log("warming C:\\taz (about 10 minutes)")
    facts = run_script(cfg, ip, IMAGE_DIR / "warm.ps1")
    timings = ", ".join(f"{k} {v} s" for k, v in facts["timings"].items())
    expiry = eval_expiry(cfg, ip)
    finish_disk(cfg, ip)
    shutdown(b, cfg)
    description = render(
        "layer.md.in",
        common_values(b)
        | key_values(b, win.KNOWN_HOSTS)
        | {"COMMIT": facts["commit"], "EVAL_EXPIRY": expiry, "TIMINGS": timings},
    )
    make_template(b, cfg, description, "build;taz;template;windows")


def verify(b: Build) -> None:
    b.log("verifying: `just test` on a clone of the new layer")
    if win.cmd_run(b.cfg, ["test"]):
        sys.exit(f"error: `just test` failed; VM {b.cfg.vmid} is left up to inspect")
    win.cmd_down(b.cfg)
    b.log(
        f"done. Paste WIN_PASSWORD into the notes of {b.base} and {b.layer} "
        "(Summary -> Notes) in place of the placeholder."
    )


def main(argv: Sequence[str] | None = None) -> None:
    args = list(sys.argv[1:] if argv is None else argv)
    if args not in (["all"], ["layer"]):
        print(__doc__)
        sys.exit(2)
    b = load_build()
    if args == ["all"]:
        build_base(b)
    build_layer(b)
    verify(b)


if __name__ == "__main__":
    main()
