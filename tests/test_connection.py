import pathlib


class LocalPath(pathlib.PurePath):
    pass


class RemotePath:
    pass


class Filesystem:
    def __init__(self, client) -> None:
        self._client = client

    def put(self, src, dest) -> RemotePath:
        pass

    def get(self, src, dest) -> LocalPath:
        pass

    def stat(self, src, dest) -> RemotePath:
        pass


class TazerClient:
    @property
    def filesystem(self) -> Filesystem:
        return Filesystem(client=self)


def test_connect() -> None:
    with TazerClient() as tazer:
        tazer.filesystem.put(src=LocalPath(...), dest=RemotePath(...))
        tazer.filesystem.get(src=RemotePath(...), dest=LocalPath(...))
        tazer.filesystem.stat(path=...)

        tazer.command.execute()
        tazer.process.list()
        tazer.process.info()
        tazer.process.monitor()
