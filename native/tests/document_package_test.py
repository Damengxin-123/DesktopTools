"""使用隔离数据验证文档打包、加密与兼容读取。"""
import base64
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import zipfile
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]

class Backend:
    """测试期间独立启动服务，通过正常本机协议访问。"""
    def __init__(self, directory, debug=False):
        self.server = socket.socket()
        self.server.bind(('127.0.0.1', 0))
        self.server.listen()
        self.server.settimeout(20)
        env = os.environ.copy()
        if debug:
            env['QTWEBENGINE_REMOTE_DEBUGGING'] = '127.0.0.1:19386'
            env['QTWEBENGINE_CHROMIUM_FLAGS'] = '--remote-allow-origins=*'
        self.process = subprocess.Popen([str(ROOT/'native/runtime/DesktopToolNative.exe'), str(self.server.getsockname()[1]), 'document-test', str(directory), '--isolated'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.socket, _ = self.server.accept()
        self.socket.settimeout(20)
        self.stream = self.socket.makefile('rb')
        assert not json.loads(self.stream.readline()).get('error')
        self.sequence = 0

    def request(self, method, *args):
        """忽略事件通知，只返回对应请求的结果。"""
        self.sequence += 1
        self.socket.sendall((json.dumps({'id': self.sequence, 'method': method, 'args': args})+'\n').encode())
        while True:
            response = json.loads(self.stream.readline())
            if response.get('id') == self.sequence:
                return response['result']

    def close(self):
        """退出隔离进程，不触碰用户正在运行的实例。"""
        self.stream.close()
        self.socket.close()
        self.server.close()
        try:
            self.process.wait(8)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(5)


def verify():
    """覆盖资源拆分、普通 ZIP、密码加解锁及旧版 HTML 迁移。"""
    import pyzipper
    with tempfile.TemporaryDirectory(prefix='dadoch-test-') as directory:
        backend = Backend(directory)
        try:
            image = io.BytesIO()
            Image.new('RGB', (8, 8), '#e4bb62').save(image, 'PNG')
            html = '<ol start="3"><li>背景</li><li><b>目标</b></li></ol><p><img src="data:image/png;base64,' + base64.b64encode(image.getvalue()).decode() + '"></p>'
            result = backend.request('saveNote', {'title': '分享文档', 'html': html, 'styles': [{'name': '预设'}]})
            assert result['ok'], result
            data = result['data']
            package = Path(data['packagePath'])
            with zipfile.ZipFile(package) as archive:
                assert 'index.html' in archive.namelist() and 'document.json' in archive.namelist()
                assert len([name for name in archive.namelist() if name.startswith('resources/')]) == 1
                assert all(info.compress_type == zipfile.ZIP_STORED and not (info.flag_bits & 1) for info in archive.infolist())
                assert b'data:image' not in archive.read('index.html')
            reopened = backend.request('getNote', data['id'])
            assert reopened['ok'] and 'data:image/png;base64,' in reopened['data']['html']
            assert reopened['data']['styles'] == [{'name': '预设'}]
            locked = backend.request('saveNote', {**data, 'password': '测试密码-123'})
            assert locked['ok'], locked
            with pyzipper.AESZipFile(package) as archive:
                assert all(info.flag_bits & 1 for info in archive.infolist())
                archive.setpassword('测试密码-123'.encode())
                assert '目标' in archive.read('index.html').decode()
                archive.setpassword(b'wrong')
                try:
                    archive.read('index.html')
                    raise AssertionError('错误密码被接受')
                except RuntimeError:
                    pass
            unlocked = backend.request('saveNote', {**data, 'password': ''})
            assert unlocked['ok'], unlocked
            assert not zipfile.ZipFile(package).getinfo('index.html').flag_bits & 1
            assert backend.request('saveNote', {**data, 'password': '测试密码-123'})['ok']
            backend.close()
            backend = Backend(directory)
            denied = backend.request('getNote', data['id'])
            assert not denied['ok'] and denied['requiresPassword']
            overwritten = backend.request('saveNote', {**data, 'password': ''})
            assert not overwritten['ok'], '未解锁却覆盖加密文档'
            print('PASS: ZIP 资源拆分、预设保存、AES 兼容、错误密码拒绝、解除密码、重启锁定')
        finally:
            backend.close()
    with tempfile.TemporaryDirectory(prefix='dadoch-legacy-') as directory:
        legacy = Path(directory)/'note'/'旧文档'
        legacy.mkdir(parents=True)
        (legacy/'index.html').write_text('<p>旧版内容</p>', encoding='utf-8')
        backend = Backend(directory)
        try:
            catalog = backend.request('getNotes')['data']['items']
            data = backend.request('getNote', catalog[0]['id'])['data']
            assert '旧版内容' in data['html']
            converted = backend.request('saveNote', data)
            assert converted['ok'], converted
            assert Path(converted['data']['packagePath']).exists()
            assert not (legacy/'index.html').exists()
            print('PASS: 旧文档读取和保存迁移')
        finally:
            backend.close()

if __name__ == '__main__':
    verify()
