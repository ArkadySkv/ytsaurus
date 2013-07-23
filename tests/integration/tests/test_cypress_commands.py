import pytest
import time

from yt_env_setup import YTEnvSetup
from yt_commands import *


##################################################################

class TestCypressCommands(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 0

    def test_root(self):
        # should not crash
        get('//@')

    def test_invalid_cases(self):
        # path not starting with /
        with pytest.raises(YTError): set('a', 20)

        # path starting with single /
        with pytest.raises(YTError): set('/a', 20)

        # empty path
        with pytest.raises(YTError): set('', 20)

        # empty token in path
        with pytest.raises(YTError): set('//tmp/a//b', 20)

        # change the type of root
        with pytest.raises(YTError): set('/', [])

        # set the root to the empty map
        # with pytest.raises(YTError): set('/', {}))

        # remove the root
        with pytest.raises(YTError): remove('/')
        # get non existent child
        with pytest.raises(YTError): get('//tmp/b')

        # remove non existent child
        with pytest.raises(YTError): remove('//tmp/b')

        # can't create entity node inside cypress
        with pytest.raises(YTError): set_str('//tmp/entity', '#')

    def test_remove(self):
        with pytest.raises(YTError): remove('//tmp/x', '--non_recursive')
        with pytest.raises(YTError): remove('//tmp/x')
        remove('//tmp/x', '--force')
        
        with pytest.raises(YTError): remove('//tmp/1', '--non_recursive')
        with pytest.raises(YTError): remove('//tmp/1')
        remove('//tmp/1', '--force')

        create("map_node", "//tmp/x/1/y", "--recursive")
        with pytest.raises(YTError): remove('//tmp/x', '--non_recursive')
        with pytest.raises(YTError): remove('//tmp/x', '--non_recursive', '--force')
        remove('//tmp/x/1/y', '--non_recursive')
        remove('//tmp/x')

    def test_list(self):
        set('//tmp/list', [1,2,"some string"])
        assert get('//tmp/list') == [1,2,"some string"]

        set('//tmp/list/end', 100)
        assert get('//tmp/list') == [1,2,"some string",100]

        set('//tmp/list/before:0', 200)
        assert get('//tmp/list') == [200,1,2,"some string",100]

        set('//tmp/list/before:0', 500)
        assert get('//tmp/list') == [500,200,1,2,"some string",100]

        set('//tmp/list/after:2', 1000)
        assert get('//tmp/list') == [500,200,1,1000,2,"some string",100]

        set('//tmp/list/3', 777)
        assert get('//tmp/list') == [500,200,1,777,2,"some string",100]

        remove('//tmp/list/4')
        assert get('//tmp/list') == [500,200,1,777,"some string",100]

        remove('//tmp/list/4')
        assert get('//tmp/list') == [500,200,1,777,100]

        remove('//tmp/list/0')
        assert get('//tmp/list') == [200,1,777,100]

        set('//tmp/list/end', 'last')
        assert get('//tmp/list') == [200,1,777,100,"last"]

        set('//tmp/list/before:0', 'first')
        assert get('//tmp/list') == ["first",200,1,777,100,"last"]

        set('//tmp/list/begin', 'very_first')
        assert get('//tmp/list') == ["very_first","first",200,1,777,100,"last"]

    def test_map(self):
        set('//tmp/map', {'hello': 'world', 'list':[0,'a',{}], 'n': 1})
        assert get('//tmp/map') == {"hello":"world","list":[0,"a",{}],"n":1}

        set('//tmp/map/hello', 'not_world')
        assert get('//tmp/map') == {"hello":"not_world","list":[0,"a",{}],"n":1}

        set('//tmp/map/list/2/some', 'value')
        assert get('//tmp/map') == {"hello":"not_world","list":[0,"a",{"some":"value"}],"n":1}

        remove('//tmp/map/n')
        assert get('//tmp/map') ==  {"hello":"not_world","list":[0,"a",{"some":"value"}]}

        set('//tmp/map/list', [])
        assert get('//tmp/map') == {"hello":"not_world","list":[]}

        set('//tmp/map/list/end', {})
        set('//tmp/map/list/0/a', 1)
        assert get('//tmp/map') == {"hello":"not_world","list":[{"a":1}]}

        set('//tmp/map/list/begin', {})
        set('//tmp/map/list/0/b', 2)
        assert get('//tmp/map') == {"hello":"not_world","list":[{"b":2},{"a":1}]}

        remove('//tmp/map/hello')
        assert get('//tmp/map') == {"list":[{"b":2},{"a":1}]}

        remove('//tmp/map/list')
        assert get('//tmp/map') == {}

    def test_attributes(self):
        set_str('//tmp/t', '<attr=100;mode=rw> {nodes=[1; 2]}')
        assert get('//tmp/t/@attr') == 100
        assert get('//tmp/t/@mode') == "rw"

        remove('//tmp/t/@*')
        with pytest.raises(YTError): get('//tmp/t/@attr')
        with pytest.raises(YTError): get('//tmp/t/@mode')

        # changing attributes
        set_str('//tmp/t/a', '< author=ignat > []')
        assert get('//tmp/t/a') == []
        assert get('//tmp/t/a/@author') == "ignat"

        set('//tmp/t/a/@author', "not_ignat")
        assert get('//tmp/t/a/@author') == "not_ignat"

        # nested attributes (actually shows <>)
        set_str('//tmp/t/b', '<dir = <file = <>-100> #> []')
        assert get_str('//tmp/t/b/@dir/@') == '{"file"=<>-100}'
        assert get_str('//tmp/t/b/@dir/@file') == '<>-100'
        assert get_str('//tmp/t/b/@dir/@file/@') == '{}'

        # set attributes directly
        set('//tmp/t/@', {'key1': 'value1', 'key2': 'value2'})
        assert get('//tmp/t/@key1') == "value1"
        assert get('//tmp/t/@key2') == "value2"

        # error cases
        # typo (extra slash)
        with pytest.raises(YTError): get('//tmp/t/@/key1')
        # change type
        with pytest.raises(YTError): set('//tmp/t/@', 1)
        with pytest.raises(YTError): set('//tmp/t/@', 'a')
        with pytest.raises(YTError): set_str('//tmp/t/@', '<>')
        with pytest.raises(YTError): set('//tmp/t/@', [1, 2, 3])

    def test_attributes_tx_read(self):
        set_str('//tmp/t', '<attr=100> 123')
        assert get('//tmp/t') == 123
        assert get('//tmp/t/@attr') == 100
        assert 'attr' in get('//tmp/t/@')

        tx = start_transaction()
        assert get('//tmp/t/@attr', tx = tx) == 100
        assert 'attr' in get('//tmp/t/@', tx = tx)

    def test_format_json(self):
        # check input format for json
        set_str('//tmp/json_in', '{"list": [1,2,{"string": "this"}]}', format="json")
        assert get('//tmp/json_in') == {"list": [1, 2, {"string": "this"}]}

        # check output format for json
        set('//tmp/json_out', {'list': [1, 2, {'string': 'this'}]})
        assert get_str('//tmp/json_out', format="json") == '{"list":[1,2,{"string":"this"}]}'

    def test_map_remove_all1(self):
        # remove items from map
        set('//tmp/map', {"a" : "b", "c": "d"})
        assert get('//tmp/map/@count') == 2
        remove('//tmp/map/*')
        assert get('//tmp/map') == {}
        assert get('//tmp/map/@count') == 0

    def test_map_remove_all2(self):
        set('//tmp/map', {'a' : 1})
        tx = start_transaction()
        set('//tmp/map', {'b' : 2}, tx = tx)
        remove('//tmp/map/*', tx = tx)
        assert get('//tmp/map', tx = tx) == {}
        assert get('//tmp/map/@count', tx = tx) == 0
        commit_transaction(tx)
        assert get('//tmp/map') == {}
        assert get('//tmp/map/@count') == 0

    def test_list_remove_all(self):
        # remove items from list
        set('//tmp/list', [10, 20, 30])
        assert get('//tmp/list/@count') == 3
        remove('//tmp/list/*')
        assert get('//tmp/list') == []
        assert get('//tmp/list/@count') == 0

    def test_attr_remove_all1(self):
        # remove items from attributes
        set_str('//tmp/attr', '<_foo=bar;_key=value>42');
        remove('//tmp/attr/@*')
        with pytest.raises(YTError): get('//tmp/attr/@_foo')
        with pytest.raises(YTError): get('//tmp/attr/@_key')

    def test_attr_remove_all2(self):
        set('//tmp/@a', 1)
        tx = start_transaction()
        set('//tmp/@b', 2, tx = tx)
        remove('//tmp/@*', tx = tx)
        with pytest.raises(YTError): get('//tmp/@a', tx = tx)
        with pytest.raises(YTError): get('//tmp/@b', tx = tx)
        commit_transaction(tx)
        with pytest.raises(YTError): get('//tmp/@a')
        with pytest.raises(YTError): get('//tmp/@b')

    def test_copy_simple1(self):
        set('//tmp/a', 1)
        copy('//tmp/a', '//tmp/b')
        assert get('//tmp/b') == 1

    def test_copy_simple2(self):
        set('//tmp/a', [1, 2, 3])
        copy('//tmp/a', '//tmp/b')
        assert get('//tmp/b') == [1, 2, 3]

    def test_copy_simple3(self):
        set_str('//tmp/a', '<x=y> 1')
        copy('//tmp/a', '//tmp/b')
        assert get('//tmp/b/@x') == 'y'

    def test_copy_simple4(self):
        set('//tmp/a', {'x1' : 'y1', 'x2' : 'y2'})
        assert get('//tmp/a/@count') == 2

        copy('//tmp/a', '//tmp/b')
        assert get('//tmp/b/@count') == 2

    def test_copy_simple5(self):
        set("//tmp/a", { 'b' : 1 })
        assert get('//tmp/a/b/@path') == '//tmp/a/b'

        copy('//tmp/a', '//tmp/c')
        assert get('//tmp/c/b/@path') == '//tmp/c/b'

        remove('//tmp/a')
        assert get('//tmp/c/b/@path') == '//tmp/c/b'

    def test_copy_tx1(self):
        tx = start_transaction()

        set('//tmp/a', {'x1' : 'y1', 'x2' : 'y2'}, tx=tx)
        assert get('//tmp/a/@count', tx=tx) == 2

        copy('//tmp/a', '//tmp/b', tx=tx)
        assert get('//tmp/b/@count', tx=tx) == 2

        commit_transaction(tx)

        assert get('//tmp/a/@count') == 2
        assert get('//tmp/b/@count') == 2

    def test_copy_tx2(self):
        set('//tmp/a', {'x1' : 'y1', 'x2' : 'y2'})

        tx = start_transaction()

        remove('//tmp/a/x1', tx=tx)
        assert get('//tmp/a/@count', tx=tx) == 1

        copy('//tmp/a', '//tmp/b', tx=tx)
        assert get('//tmp/b/@count', tx=tx) == 1

        commit_transaction(tx)

        assert get('//tmp/a/@count') == 1
        assert get('//tmp/b/@count') == 1

    def test_copy_unexisting_path(self):
        with pytest.raises(YTError): copy('//tmp/x', '//tmp/y')

    def test_copy_cannot_have_children(self):
        create('table', '//tmp/t1')
        create('table', '//tmp/t2')
        with pytest.raises(YTError): copy('//tmp/t2', '//tmp/t1/xxx')

    def test_copy_table_compression_codec(self):
        create('table', '//tmp/t1')
        assert get('//tmp/t1/@compression_codec') == 'lz4'
        set('//tmp/t1/@compression_codec', 'gzip_normal')
        copy('//tmp/t1', '//tmp/t2')
        assert get('//tmp/t2/@compression_codec') == 'gzip_normal'

    def test_copy_id1(self):
        set('//tmp/a', 123)
        a_id = get('//tmp/a/@id')
        copy('#' + a_id, '//tmp/b')
        assert get('//tmp/b') == 123

    def test_copy_id2(self):
        set('//tmp/a', 123)
        tmp_id = get('//tmp/@id')
        copy('#' + tmp_id + '/a', '//tmp/b')
        assert get('//tmp/b') == 123

    def test_move_simple(self):
        set('//tmp/a', 1)
        move('//tmp/a', '//tmp/b')
        assert get('//tmp/b') == 1
        with pytest.raises(YTError): get('//tmp/a')

    def test_remove_locks(self):
        set('//tmp/a', {'b' : 1})

        tx1 = start_transaction()
        tx2 = start_transaction()

        set('//tmp/a/b', 2, tx = tx1)
        with pytest.raises(YTError): remove('//tmp/a', tx = tx2)

    def test_map_locks1(self):
        tx = start_transaction()
        set('//tmp/a', 1, tx = tx)
        assert get('//tmp/@lock_mode') == 'none'
        assert get('//tmp/@lock_mode', tx = tx) == 'shared'

        locks = get('//tmp/@locks', tx = tx)
        assert len(locks) == 1

        lock = locks[0]
        assert lock['mode'] == 'shared'
        assert lock['child_keys'] == ['a']

        commit_transaction(tx)
        assert get('//tmp') == {'a' : 1}

    def test_map_locks2(self):
        tx1 = start_transaction()
        set('//tmp/a', 1, tx = tx1)

        tx2 = start_transaction()
        set('//tmp/b', 2, tx = tx2)

        assert get('//tmp', tx = tx1) == {'a' : 1}
        assert get('//tmp', tx = tx2) == {'b' : 2}
        assert get('//tmp') == {}

        commit_transaction(tx1)
        assert get('//tmp') == {'a' : 1}
        assert get('//tmp', tx = tx2) == {'a' : 1, 'b' : 2}

        commit_transaction(tx2)
        assert get('//tmp') == {'a' : 1, 'b' : 2}

    def test_map_locks3(self):
        tx1 = start_transaction()
        set('//tmp/a', 1, tx = tx1)

        tx2 = start_transaction()
        with pytest.raises(YTError): set('//tmp/a', 2, tx = tx2)

    def test_map_locks4(self):
        set('//tmp/a', 1)

        tx = start_transaction()
        remove('//tmp/a', tx = tx)

        assert get('//tmp/@lock_mode', tx = tx) == 'shared'

        locks = get('//tmp/@locks', tx = tx)
        assert len(locks) == 1

        lock = locks[0]
        assert lock['mode'] == 'shared'
        assert lock['child_keys'] == ['a']

    def test_map_locks5(self):
        set('//tmp/a', 1)

        tx1 = start_transaction()
        remove('//tmp/a', tx = tx1)

        tx2 = start_transaction()
        with pytest.raises(YTError): set('//tmp/a', 2, tx = tx2)

    def test_map_locks6(self):
        tx = start_transaction()
        set('//tmp/a', 1, tx = tx)
        assert get('//tmp/a', tx = tx) == 1
        assert get('//tmp') == {}

        with pytest.raises(YTError): remove('//tmp/a')
        remove('//tmp/a', tx = tx)
        assert get('//tmp', tx = tx) == {}

        commit_transaction(tx)
        assert get('//tmp') == {}

    def test_map_locks7(self):
        set('//tmp/a', 1)

        tx = start_transaction()
        remove('//tmp/a', tx = tx)
        set('//tmp/a', 2, tx = tx)
        remove('//tmp/a', tx = tx)
        commit_transaction(tx)

        assert get('//tmp') == {}

    def test_attr_locks1(self):
        tx = start_transaction()
        set('//tmp/@a', 1, tx = tx)
        assert get('//tmp/@lock_mode') == 'none'
        assert get('//tmp/@lock_mode', tx = tx) == 'shared'

        locks = get('//tmp/@locks', tx = tx)
        assert len(locks) == 1

        lock = locks[0]
        assert lock['mode'] == 'shared'
        assert lock['attribute_keys'] == ['a']

        commit_transaction(tx)
        assert get('//tmp/@a') == 1

    def test_attr_locks2(self):
        tx1 = start_transaction()
        set('//tmp/@a', 1, tx = tx1)

        tx2 = start_transaction()
        set('//tmp/@b', 2, tx = tx2)

        assert get('//tmp/@a', tx = tx1) == 1
        assert get('//tmp/@b', tx = tx2) == 2
        with pytest.raises(YTError): get('//tmp/@a')
        with pytest.raises(YTError): get('//tmp/@b')

        commit_transaction(tx1)
        assert get('//tmp/@a') == 1
        assert get('//tmp/@a', tx = tx2) == 1
        assert get('//tmp/@b', tx = tx2) == 2

        commit_transaction(tx2)
        assert get('//tmp/@a') == 1
        assert get('//tmp/@b') == 2

    def test_attr_locks3(self):
        tx1 = start_transaction()
        set('//tmp/@a', 1, tx = tx1)

        tx2 = start_transaction()
        with pytest.raises(YTError): set('//tmp/@a', 2, tx = tx2)

    def test_attr_locks4(self):
        set('//tmp/@a', 1)

        tx = start_transaction()
        remove('//tmp/@a', tx = tx)

        assert get('//tmp/@lock_mode', tx = tx) == 'shared'

        locks = get('//tmp/@locks', tx = tx)
        assert len(locks) == 1

        lock = locks[0]
        assert lock['mode'] == 'shared'
        assert lock['attribute_keys'] == ['a']

    def test_attr_locks5(self):
        set('//tmp/@a', 1)

        tx1 = start_transaction()
        remove('//tmp/@a', tx = tx1)

        tx2 = start_transaction()
        with pytest.raises(YTError): set('//tmp/@a', 2, tx = tx2)

    def test_attr_locks6(self):
        tx = start_transaction()
        set('//tmp/@a', 1, tx = tx)
        assert get('//tmp/@a', tx = tx) == 1
        with pytest.raises(YTError): get('//tmp/@a')

        with pytest.raises(YTError): remove('//tmp/@a')
        remove('//tmp/@a', tx = tx)
        with pytest.raises(YTError): get('//tmp/@a', tx = tx)

        commit_transaction(tx)
        with pytest.raises(YTError): get('//tmp/@a')

    def test_embedded_attributes(self):
        set("//tmp/a", {})
        set("//tmp/a/@attr", {"key": "value"})
        set("//tmp/a/@attr/key/@embedded_attr", "emb")
        assert get_str("//tmp/a/@attr") == '{"key"=<"embedded_attr"="emb">"value"}'
        assert get_str("//tmp/a/@attr/key") == '<"embedded_attr"="emb">"value"'
        assert get_str("//tmp/a/@attr/key/@embedded_attr") == '"emb"'

    def test_get_with_attributes(self):
        set('//tmp/a', {})
        assert get_str('//tmp', attr=['type']) == '<"type"="map_node">{"a"=<"type"="map_node">{}}'

    def test_list_with_attributes(self):
        set('//tmp/a', {})
        assert ls_str('//tmp', attr=['type']) == '[<"type"="map_node">"a"]'

    def test_get_list_with_attributes_virtual_maps(self):
        tx = start_transaction()
        assert get_str('//sys/transactions', attr=['state']) == '{"%s"=<"state"="active">#}' % tx
        assert ls_str('//sys/transactions', attr=['state']) == '[<"state"="active">"%s"]' % tx
        abort_transaction(tx)

    def test_exists(self):
        assert exists("//tmp")
        assert not exists("//tmp/a")
        assert not exists("//tmp/a/f/e")
        assert not exists("//tmp/a/1/e")
        assert not exists("//tmp/a/2/1")

        set("//tmp/1", {})
        assert exists("//tmp/1")
        assert not exists("//tmp/1/2")

        set("//tmp/a", {})
        assert exists("//tmp/a")

        set("//tmp/a/@list", [10])
        assert exists("//tmp/a/@list")
        assert exists("//tmp/a/@list/0")
        assert not exists("//tmp/a/@list/1")

        assert not exists("//tmp/a/@attr")
        set("//tmp/a/@attr", {"key": "value"})
        assert exists("//tmp/a/@attr")

        assert exists("//sys/operations")
        assert exists("//sys/transactions")
        assert not exists("//sys/xxx")
        assert not exists("//sys/operations/xxx")

    def test_remove_tx1(self):
        set('//tmp/a', 1)
        assert get('//tmp/@id') == get('//tmp/a/@parent_id')
        tx = start_transaction()
        remove('//tmp/a', tx=tx)
        assert get('//tmp/@id') == get('//tmp/a/@parent_id')
        abort_transaction(tx)
        assert get('//tmp/@id') == get('//tmp/a/@parent_id')

    def test_create(self):
        remove("//tmp/*")
        create("map_node", "//tmp/some_node")

        with pytest.raises(YTError): create("map_node", "//tmp/a/b")
        create("map_node", "//tmp/a/b", "--recursive")

        with pytest.raises(YTError): create("map_node", "//tmp/a/b")
        create("map_node", "//tmp/a/b", "--ignore_existing")

        with pytest.raises(YTError): create("table", "//tmp/a/b", "--ignore_existing")

    def test_link1(self):
        with pytest.raises(YTError): link("//tmp/a", "//tmp/b")

    def test_link2(self):
        set("//tmp/t1", 1)
        link("//tmp/t1", "//tmp/t2")
        assert get("//tmp/t2") == 1
        assert get("//tmp/t2/@type") == "integer_node"
        assert get("//tmp/t1/@id") == get("//tmp/t2/@id")
        assert get("//tmp/t2&/@type") == "link"
        assert get("//tmp/t2&/@broken") == "false"

        set("//tmp/t1", 2)
        assert get("//tmp/t2") == 2

    def test_link3(self):
        set("//tmp/t1", 1)
        link("//tmp/t1", "//tmp/t2")
        remove("//tmp/t1")
        assert get("//tmp/t2&/@broken") == "true"

    def test_link4(self):
        set("//tmp/t1", 1)
        link("//tmp/t1", "//tmp/t2")

        tx = start_transaction()
        id = get("//tmp/t1/@id")
        lock("#%s" % id, mode = "snapshot", tx = tx)

        remove("//tmp/t1")
        
        assert get("#%s" % id, tx = tx) == 1
        assert get("//tmp/t2&/@broken") == "true"
        with pytest.raises(YTError): read("//tmp/t2")

    def test_link5(self):
        set("//tmp/t1", 1)
        set("//tmp/t2", 2)
        with pytest.raises(YTError): link("//tmp/t1", "//tmp/t2")

    def test_link6(self):
        create("table", "//tmp/a")
        link("//tmp/a", "//tmp/b")

        assert exists("//tmp/a")
        assert exists("//tmp/b")
        assert exists("//tmp/b&")
        assert exists("//tmp/b/@id")
        assert exists("//tmp/b/@row_count")
        assert exists("//tmp/b&/@target_id")
        assert not exists("//tmp/b/@x")
        assert not exists("//tmp/b/x")
        assert not exists("//tmp/b&/@x")
        assert not exists("//tmp/b&/x")

        remove("//tmp/a")
        
        assert not exists("//tmp/a")
        assert not exists("//tmp/b")
        assert exists("//tmp/b&")
        assert not exists("//tmp/b/@id")
        assert not exists("//tmp/b/@row_count")
        assert exists("//tmp/b&/@target_id")
        assert not exists("//tmp/b/@x")
        assert not exists("//tmp/b/x")
        assert not exists("//tmp/b&/@x")
        assert not exists("//tmp/b&/x")

    def test_access_stat1(self):
        c1 = get('//tmp/@access_counter')
        time.sleep(2.0)
        c2 = get('//tmp/@access_counter')
        assert c2 == c1 + 1

    def test_access_stat2(self):
        c1 = get('//tmp/@access_counter')
        tx = start_transaction()
        lock('//tmp', mode = 'snapshot', tx = tx)
        time.sleep(2.0)
        c2 = get('//tmp/@access_counter', tx = tx)
        assert c2 == c1 + 2
