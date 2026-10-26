"""Local upstream protocol fixture; never calls a model or modifies a project."""
import json
import sys
from pathlib import Path

scenario = sys.argv[1] if len(sys.argv) > 1 else 'normal'
pending = None
room_instructions = ''
role = 'gpt'


def send(value):
    print(json.dumps(value), flush=True)


def complete(turn_id, message):
    item = {'type': 'agentMessage', 'id': 'msg-1', 'phase': 'final_answer', 'text': message}
    send({'method': 'item/completed', 'params': {'threadId': 'thread-1', 'turnId': turn_id, 'item': item}})
    send({'method': 'turn/completed', 'params': {'threadId': 'thread-1', 'turn': {'id': turn_id, 'status': 'completed', 'items': [item]}}})


for line in sys.stdin:
    request = json.loads(line)
    method, params = request.get('method'), request.get('params', {})
    if 'result' in request:
        complete('turn-1', request['result'].get('decision', 'unknown'))
        continue
    if method == 'initialize':
        send({'id': request['id'], 'result': {'serverInfo': {'name': 'fixture'}}})
    elif method == 'thread/start':
        room_instructions = params.get('developerInstructions', '')
        role = 'deepseek' if '你是 DeepSeek' in room_instructions else 'gpt'
        if scenario == 'startup-error':
            send({'id': request['id'], 'error': {'code': -32000, 'message': 'cannot start'}})
        elif scenario == 'provider-config' and (params.get('modelProvider') != 'deepseek-menu' or params.get('model') != 'fixture-deepseek' or params.get('config', {}).get('model_providers', {}).get('deepseek-menu', {}).get('wire_api') != 'responses'):
            send({'id': request['id'], 'error': {'code': -32000, 'message': 'provider configuration missing'}})
        else:
            send({'id': request['id'], 'result': {'thread': {'id': 'thread-1', 'modelProvider': params.get('modelProvider', 'openai')}}})
    elif method == 'turn/start':
        pending = 'turn-1'
        if scenario == 'delayed-start':
            continue
        send({'method': 'turn/started', 'params': {'threadId': 'thread-1', 'turn': {'id': pending, 'status': 'inProgress', 'items': []}}})
        send({'id': request['id'], 'result': {'turn': {'id': pending, 'status': 'inProgress', 'items': []}}})
        if scenario == 'hang':
            continue
        if scenario == 'approval':
            send({'id': 'approval-1', 'method': 'item/commandExecution/requestApproval', 'params': {'threadId': 'thread-1', 'turnId': pending, 'command': 'touch file', 'cwd': '/tmp', 'itemId': 'cmd-1', 'approvalId': 'a1', 'reason': 'write request'}})
        elif scenario == 'failed-turn':
            send({'method': 'turn/completed', 'params': {'threadId': 'thread-1', 'turn': {'id': pending, 'status': 'failed', 'items': [], 'error': {'message': 'provider failed'}}}})
        else:
            send({'method': 'item/started', 'params': {'threadId': 'thread-1', 'turnId': pending, 'item': {'id': 'cmd-1', 'type': 'commandExecution', 'command': 'pwd', 'cwd': '/tmp', 'status': 'inProgress'}}})
            if scenario == 'team':
                prompt = params['input'][0]['text']
                if role == 'gpt' and params['sandboxPolicy']['type'] == 'workspaceWrite' and prompt == '创建一个文件并检查':
                    Path('artifact.txt').write_text('one executor\n')
                    complete(pending, '已创建 artifact.txt。\n[[next:deepseek]]')
                elif role == 'deepseek' and Path('artifact.txt').is_file():
                    complete(pending, '检查通过：' + Path('artifact.txt').read_text().strip() + '\n[[next:user]]')
                else:
                    complete(pending, '已收到。\n[[next:user]]')
            elif scenario == 'instructions':
                complete(pending, room_instructions)
            elif scenario == 'provider-config':
                complete(pending, 'provider-ok')
            elif scenario == 'scope':
                complete(pending, params.get('cwd', '') + '::' + ','.join(params['sandboxPolicy'].get('writableRoots', [])))
            else:
                complete(pending, params['sandboxPolicy']['type'] + ':' + params['input'][0]['text'])
    elif method == 'turn/steer':
        send({'id': request['id'], 'result': {'turnId': pending}})
    elif method == 'turn/interrupt':
        send({'id': request['id'], 'result': {}})
        send({'method': 'turn/completed', 'params': {'threadId': 'thread-1', 'turn': {'id': pending, 'status': 'interrupted', 'items': []}}})
