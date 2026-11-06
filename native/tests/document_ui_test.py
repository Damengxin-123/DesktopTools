"""通过 Qt WebEngine 调试接口验证编辑界面，无需屏幕控制。"""
import base64
import json
from pathlib import Path
import tempfile
import time
import urllib.request
import websocket
from document_package_test import Backend, ROOT

# 使用真实原生窗口与隔离数据，检查布局、导航、样式和保存重开。
with tempfile.TemporaryDirectory(prefix='dadoch-ui-') as directory:
    backend = Backend(directory, debug=True)
    try:
        data = backend.request('saveNote', {'title': '海滨新城 · 设计文档', 'html': '<ol><li>项目概述</li><li>城市规划<ol><li>公共空间</li></ol></li></ol><p>这是连续的文档主体，可直接从上到下编辑。</p><p>选中段落或文字后，可使用右侧样式快速排版。</p>'})['data']
        assert backend.request('openDocument', data['id'], 'default', 'gold')['ok']
        targets = []
        for _ in range(60):
            try:
                targets = json.load(urllib.request.urlopen('http://127.0.0.1:19386/json'))
                if targets: break
            except OSError: pass
            time.sleep(.1)
        ws = websocket.create_connection(targets[0]['webSocketDebuggerUrl'], timeout=15, suppress_origin=True)
        sequence = 0
        def command(method, params):
            """等待指定调试请求完成，忽略页面事件。"""
            global sequence
            sequence += 1
            ws.send(json.dumps({'id': sequence, 'method': method, 'params': params}))
            while True:
                response = json.loads(ws.recv())
                if response.get('id') == sequence:
                    assert 'error' not in response, response
                    return response.get('result', {})
        def evaluate(expression):
            """执行页面测试表达式，并将异常视为失败。"""
            result = command('Runtime.evaluate', {'expression': expression, 'returnByValue': True, 'awaitPromise': True})
            assert 'exceptionDetails' not in result, result
            return result.get('result', {}).get('value')
        for _ in range(80):
            if evaluate('typeof revision !== "undefined" && revision > 0'): break
            time.sleep(.1)
        outline = evaluate('[...document.querySelectorAll("#outline button")].map(x=>x.textContent)')
        assert len(outline) == 3, outline
        assert evaluate('document.querySelector("#editor").getBoundingClientRect().width > 400')
        assert evaluate('getComputedStyle(editor).maxWidth === "none"')
        evaluate('document.querySelector("#outline button").click(); applyPreset(1)')
        assert evaluate('editor.querySelector("li").innerHTML.includes("font")'), evaluate('editor.innerHTML')
        evaluate('byId("edit-styles").click(); byId("preset-name").value="自定义正文"; byId("styles-dialog").querySelector("form").requestSubmit()')
        evaluate('save()')
        for _ in range(80):
            if evaluate('!saving && saved === changes'): break
            time.sleep(.1)
        assert evaluate('saved === changes && !saving'), evaluate('status.textContent')
        stored = backend.request('getNote', data['id'])
        assert stored['ok'] and stored['data']['styles'][0]['name'] == '自定义正文', stored
        assert '项目概述' in stored['data']['html']
        assert backend.request('openDocument', '', 'default', 'gold')['ok']
        assert backend.request('openDocument', data['id'], 'default', 'gold')['ok']
        time.sleep(.3)
        assert evaluate('presets[0].name') == '自定义正文'
        assert evaluate('document.querySelectorAll("#outline button").length') == 3
        # 选中文字模式只改变所选文本，连续长文可滚动且不产生分页节点。
        evaluate('byId("style-scope").value="selection"; const para=editor.querySelector("p"); const first=para.firstChild; const selected=document.createRange(); selected.setStart(first,0); selected.setEnd(first,2); restoreSelection(selected); rememberSelection(); applyPreset(4)')
        assert evaluate('editor.querySelector("p").querySelector("span") !== null')
        evaluate('format("undo"); format("redo"); appearance("dark")')
        assert evaluate('document.documentElement.dataset.theme') == 'dark'
        evaluate('appearance("gold")')
        command('Emulation.setDeviceMetricsOverride', {'width': 900, 'height': 700, 'deviceScaleFactor': 1, 'mobile': False})
        assert evaluate('document.documentElement.scrollWidth <= 900')
        assert evaluate('editor.getBoundingClientRect().width > 400')
        command('Emulation.clearDeviceMetricsOverride', {})
        screenshot = command('Page.captureScreenshot', {'format':'png'})['data']
        (ROOT/'native/build/document-layout.png').write_bytes(base64.b64decode(screenshot))
        print('PASS: 三栏布局、自适应正文、编号导航、样式应用、预设编辑和保存重开')
        evaluate('host.setDirty(revision, false)')
        ws.close()
    finally:
        backend.close()
