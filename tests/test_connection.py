import pathlib

class LocalPath(pathlib.PurePath):
    pass

class RemotePath():
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

class TazerClient():
    @property
    def filesystem(self) -> Filesystem:
        return Filesystem(client=self)

def test_connect() -> None:
    with TazerClient() as tazer:
        rf = tazer.filesystem.put(src=LocalPath(...), dest=RemotePath(...))
        lf = tazer.filesystem.get(src=RemotePath(...), dest=LocalPath(...))
        f = tazer.filesystem.stat(path=...)

        resp = tazer.command.execute()
        procs = tazer.process.list()
        proc = tazer.process.info()
        tazer.process.monitor()
