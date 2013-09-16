import yt.yson as yson
from yt.driver import Driver, Request, make_request
from yt.common import YtError, flatten, update

import os
import sys

import time
from datetime import datetime

from cStringIO import StringIO

def get_driver():
    config_path = os.environ['YT_CONFIG']
    if not hasattr(get_driver, "driver") or (hasattr(get_driver, "config_path") and config_path != get_driver.config_path):
        get_driver.driver = Driver(config=yson.loads(open(config_path).read()))
        get_driver.config_path = config_path
    return get_driver.driver

def set_branch(dict, path, value):
    root = dict
    for field in path[:-1]:
        if field not in root:
            root[field] = {}
        root = root[field]
    root[path[-1]] = value

def change(dict, old, new):
    if old in dict:
        set_branch(dict, flatten(new), dict[old])
        del dict[old]

def flat(dict, key):
    if key in dict:
        dict[key] = flatten(dict[key])

def prepare_path(path):
    attributes = {}
    if isinstance(path, yson.YsonString):
        attributes = path.attributes
    result = yson.loads(command("parse_ypath", arguments={"path": path}, verbose=False))
    update(result.attributes, attributes)
    return result

def prepare_paths(paths):
    return [prepare_path(path) for path in flatten(paths)]

def prepare_args(arguments):
    change(arguments, "tx", "transaction_id")
    change(arguments, "attr", "attributes")
    change(arguments, "ping_ancestor_txs", "ping_ancestor_transactions")
    if "opt" in arguments:
        for option in flatten(arguments["opt"]):
            key, value = option.split("=", 1)
            set_branch(arguments, key.strip("/").split("/"), yson.loads(value))
        del arguments["opt"]

    return arguments

def command(command_name, arguments, input_stream=None, output_stream=None, verbose=None):
    if "verbose" in arguments:
        verbose = arguments["verbose"]
        del arguments["verbose"]

    user = None
    if "user" in arguments:
        user = arguments["user"]
        del arguments["user"]

    if "path" in arguments and command_name != "parse_ypath":
        arguments["path"] = prepare_path(arguments["path"])

    arguments = prepare_args(arguments)

    if verbose is None or verbose:
        print >>sys.stderr, str(datetime.now()), command_name, arguments

    return make_request(
        get_driver(),
        Request(command_name=command_name,
                arguments=arguments,
                input_stream=input_stream,
                output_stream=output_stream,
                user=user))

###########################################################################

def lock(path, waitable=False, **kwargs):
    kwargs["path"] = path
    kwargs["waitable"] = waitable
    return command('lock', kwargs)

def get_str(path, **kwargs):
    kwargs["path"] = path
    return command('get', kwargs)

def remove(path, **kwargs):
    kwargs["path"] = path
    return command('remove', kwargs)

def set_str(path, value, **kwargs):
    kwargs["path"] = path
    return command('set', kwargs, input_stream=StringIO(value))

def ls_str(path, **kwargs):
    kwargs["path"] = path
    return command('list', kwargs)

def create(object_type, path, **kwargs):
    kwargs["type"] = object_type
    kwargs["path"] = path
    return command('create', kwargs)

def copy(source_path, destination_path, **kwargs):
    kwargs["source_path"] = source_path
    kwargs["destination_path"] = destination_path
    return command('copy', kwargs)

def move(source_path, destination_path, **kwargs):
    kwargs["source_path"] = source_path
    kwargs["destination_path"] = destination_path
    return command('move', kwargs)

def link(target_path, link_path, **kwargs):
    kwargs["target_path"] = target_path
    kwargs["link_path"] = link_path
    return command('link', kwargs)

def exists(path, **kwargs):
    kwargs["path"] = path
    res = command('exists', kwargs)
    return yson.loads(res) == 'true'

def read_str(path, **kwargs):
    kwargs["path"] = path
    if "output_format" not in kwargs:
        kwargs["output_format"] = yson.loads("<format=text>yson")
    output = StringIO()
    command('read', kwargs, output_stream=output)
    return output.getvalue();

def write_str(path, value, **kwargs):
    attributes = {}
    if "sorted_by" in kwargs:
        attributes={"sorted_by": flatten(kwargs["sorted_by"])}
    kwargs["path"] = yson.to_yson_type(path, attributes=attributes)
    return command('write', kwargs, input_stream=StringIO(value))

def start_transaction(**kwargs):
    out = command('start_tx', kwargs)
    return out.replace('"', '').strip('\n')

def commit_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    return command('commit_tx', kwargs)

def ping_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    return command('ping_tx', kwargs)

def abort_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    return command('abort_tx', kwargs)

def upload(path, data, **kwargs):
    kwargs["path"] = path
    return command('upload', kwargs, input_stream=StringIO(data))

def upload_file(path, file_name, **kwargs):
    with open(file_name, 'rt') as f:
        return upload(path, f.read(), **kwargs)

def download(path, **kwargs):
    kwargs["path"] = path
    output = StringIO()
    command('download', kwargs, output_stream=output)
    return output.getvalue();

def track_op(op_id):
    counter = 0
    while True:
        state = get("//sys/operations/%s/@state" % op_id, verbose=False)
        message = "Operation %s %s" % (op_id, state)
        if counter % 30 == 0 or state in ["failed", "aborted", "completed"]:
            print >>sys.stderr, message
        if state == "failed":
            error = get_str("//sys/operations/%s/@result/error" % op_id, verbose=False)
            jobs = get("//sys/operations/%s/jobs" % op_id, verbose=False)
            for job in jobs:
                if exists("//sys/operations/%s/jobs/%s/@error" % (op_id, job), verbose=False):
                    error = error + "\n\n" + get_str("//sys/operations/%s/jobs/%s/@error" % (op_id, job), verbose=False)
                    if "stderr" in jobs[job]:
                        error = error + "\n" + download("//sys/operations/%s/jobs/%s/stderr" % (op_id, job), verbose=False)
            raise YtError(error)
        if state == "aborted":
            raise YtError(message)
        if state == "completed":
            break
        counter += 1
        time.sleep(0.1)

def start_op(op_type, **kwargs):
    op_name = None
    if op_type == "map":
        op_name = "mapper"
    if op_type == "reduce":
        op_name = "reducer"

    input_name = None
    if op_type != "erase":
        kwargs["in_"] = prepare_paths(kwargs["in_"])
        input_name = "input_table_paths"

    output_name = None
    if op_type in ["map", "reduce", "map_reduce"]:
        kwargs["out"] = prepare_paths(kwargs["out"])
        output_name = "output_table_paths"
    elif "out" in kwargs:
        kwargs["out"] = prepare_path(kwargs["out"])
        output_name = "output_table_path"

    if "file" in kwargs:
        kwargs["file"] = prepare_paths(kwargs["file"])

    for opt in ["sort_by", "reduce_by"]:
        flat(kwargs, opt)

    change(kwargs, "table_path", ["spec", "table_path"])
    change(kwargs, "in_", ["spec", input_name])
    change(kwargs, "out", ["spec", output_name])
    change(kwargs, "command", ["spec", op_name, "command"])
    change(kwargs, "file", ["spec", op_name, "file_paths"])
    change(kwargs, "sort_by", ["spec","sort_by"])
    change(kwargs, "reduce_by", ["spec","reduce_by"])
    change(kwargs, "mapper_file", ["spec", "mapper", "file_paths"])
    change(kwargs, "reducer_file", ["spec", "reducer", "file_paths"])
    change(kwargs, "mapper_command", ["spec", "mapper", "command"])
    change(kwargs, "reducer_command", ["spec", "reducer", "command"])

    track = not kwargs.get("dont_track", False)
    if "dont_track" in kwargs:
        del kwargs["dont_track"]

    op_id = command(op_type, kwargs).replace('"', '')

    if track:
        track_op(op_id)

    return op_id

def map(**kwargs):
    return start_op('map', **kwargs)

def merge(**kwargs):
    flat(kwargs, "merge_by")
    for opt in ["combine_chunks", "merge_by", "mode"]:
        change(kwargs, opt, ["spec", opt])
    return start_op('merge', **kwargs)

def reduce(**kwargs):
    return start_op('reduce', **kwargs)

def map_reduce(**kwargs):
    return start_op('map_reduce', **kwargs)

def erase(path, **kwargs):
    kwargs["table_path"] = path
    change(kwargs, "combine_chunks", ["spec", "combine_chunks"])
    return start_op('erase', **kwargs)

def sort(**kwargs):
    return start_op('sort', **kwargs)

def abort_op(op, **kwargs):
    kwargs["operation_id"] = op
    command('abort_op', kwargs)

def build_snapshot(*args, **kwargs):
    get_driver().build_snapshot(*args, **kwargs)

def gc_collect():
    get_driver().gc_collect()

def create_account(name):
    command('create', {'type': 'account', 'attributes': {'name': name}})

def remove_account(name):
    remove('//sys/accounts/' + name)
    gc_collect()

def create_user(name):
    command('create', {'type': 'user', 'attributes': {'name': name}})

def remove_user(name):
    remove('//sys/users/' + name)
    gc_collect()

def create_group(name):
    command('create', {'type': 'group', 'attributes': {'name': name}})

def remove_group(name):
    remove('//sys/groups/' + name)
    gc_collect()

def add_member(member, group):
    command('add_member', {"member": member, "group": group})

def remove_member(member, group):
    command('remove_member', {"member": member, "group": group})

#########################################

def get(path, **kwargs):
    return yson.loads(get_str(path, **kwargs))

def ls(path, **kwargs):
    return yson.loads(ls_str(path, **kwargs))

def set(path, value, **kwargs):
    return set_str(path, yson.dumps(value), **kwargs)

def read(path, **kwargs):
    return yson.loads(read_str(path, **kwargs), yson_type="list_fragment")

def write(path, value, **kwargs):
    output = yson.dumps(value)
    if isinstance(value, list):
        # remove surrounding [ ]
        output = output[1:-1]
    return write_str(path, output, **kwargs)

#########################################
# Helpers:

def get_transactions():
    gc_collect()
    return ls('//sys/transactions')

def get_topmost_transactions():
    gc_collect()
    return ls('//sys/topmost_transactions')

def get_chunks():
    gc_collect()
    return ls('//sys/chunks')

def get_accounts():
    gc_collect()
    return ls('//sys/accounts')

def get_users():
    gc_collect()
    return ls('//sys/users')

def get_groups():
    gc_collect()
    return ls('//sys/groups')

#########################################
