import pytest

from yt_env_setup import YTEnvSetup
from yt_commands import *
from time import sleep


##################################################################

class TestTableCommands(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 5

    # test that chunks are not available from chunk_lists
    # Issue #198
    def test_chunk_ids(self):
        create('table', '//tmp/t')
        write_str('//tmp/t', '{a=10}')

        chunk_ids = get_chunks()
        assert len(chunk_ids) == 1
        chunk_id = chunk_ids[0]

        with pytest.raises(YTError): get('//sys/chunk_lists/"' + chunk_id + '"')

    def test_simple(self):
        create('table', '//tmp/table')

        assert read('//tmp/table') == []
        assert get('//tmp/table/@row_count') == 0
        assert get('//tmp/table/@chunk_count') == 0

        write_str('//tmp/table', '{b="hello"}')
        assert read('//tmp/table') == [{"b":"hello"}]
        assert get('//tmp/table/@row_count') == 1
        assert get('//tmp/table/@chunk_count') == 1

        write_str('<append=true>//tmp/table', '{b="2";a="1"};{x="10";y="20";a="30"}')
        assert read('//tmp/table') == [{"b": "hello"}, {"a":"1", "b":"2"}, {"a":"30", "x":"10", "y":"20"}]
        assert get('//tmp/table/@row_count') == 3
        assert get('//tmp/table/@chunk_count') == 2

    def test_sorted_write(self):
        create('table', '//tmp/table')

        write_str('//tmp/table', '{key = 0}; {key = 1}; {key = 2}; {key = 3}', sorted_by='key')

        assert get('//tmp/table/@sorted') ==  'true'
        assert get('//tmp/table/@sorted_by') ==  ['key']
        assert get('//tmp/table/@row_count') ==  4

        # sorted flag is discarded when writing to sorted table
        write_str('<append=true>//tmp/table', '{key = 4}')
        assert get('//tmp/table/@sorted') ==  'false'
        with pytest.raises(YTError): get('//tmp/table/@sorted_by')

    def test_invalid_cases(self):
        create('table', '//tmp/table')

        # we can write only list fragments
        with pytest.raises(YTError): write_str('<append=true>//tmp/table', 'string')
        with pytest.raises(YTError): write_str('<append=true>//tmp/table', '100')
        with pytest.raises(YTError): write_str('<append=true>//tmp/table', '3.14')
        with pytest.raises(YTError): write_str('<append=true>//tmp/table', '<>')

        # we can write sorted data only to empty table
        write('<append=true>//tmp/table', {'foo': 'bar'}, sorted_by='foo')
        with pytest.raises(YTError):
            write('"<append=true>" + //tmp/table', {'foo': 'zzz_bar'}, sorted_by='foo')


    def test_row_index_selector(self):
        create('table', '//tmp/table')

        write_str('//tmp/table', '{a = 0}; {b = 1}; {c = 2}; {d = 3}')

        # closed ranges
        assert read('//tmp/table[#0:#2]') == [{'a': 0}, {'b' : 1}] # simple
        assert read('//tmp/table[#-1:#1]') == [{'a': 0}] # left < min
        assert read('//tmp/table[#2:#5]') == [{'c': 2}, {'d': 3}] # right > max
        assert read('//tmp/table[#-10:#-5]') == [] # negative indexes

        assert read('//tmp/table[#1:#1]') == [] # left = right
        assert read('//tmp/table[#3:#1]') == [] # left > right

        # open ranges
        assert read('//tmp/table[:]') == [{'a': 0}, {'b' : 1}, {'c' : 2}, {'d' : 3}]
        assert read('//tmp/table[:#3]') == [{'a': 0}, {'b' : 1}, {'c' : 2}]
        assert read('//tmp/table[#2:]') == [{'c' : 2}, {'d' : 3}]

        # reading key selectors from unsorted table
        with pytest.raises(YTError): read('//tmp/table[:a]')

    def test_row_key_selector(self):
        create('table', '//tmp/table')

        v1 = {'s' : 'a', 'i': 0,    'd' : 15.5}
        v2 = {'s' : 'a', 'i': 10,   'd' : 15.2}
        v3 = {'s' : 'b', 'i': 5,    'd' : 20.}
        v4 = {'s' : 'b', 'i': 20,   'd' : 20.}
        v5 = {'s' : 'c', 'i': -100, 'd' : 10.}

        values = [v1, v2, v3, v4, v5]
        write('//tmp/table', values, sorted_by='s;i;d')

        # possible empty ranges
        assert read('//tmp/table[a : a]') == []
        assert read('//tmp/table[b : a]') == []
        assert read('//tmp/table[(c, 0) : (a, 10)]') == []
        assert read('//tmp/table[(a, 10, 1e7) : (b, )]') == []

        # some typical cases
        assert read('//tmp/table[(a, 4) : (b, 20, 18.)]') == [v2, v3]
        assert read('//tmp/table[c:]') == [v5]
        assert read('//tmp/table[:(a, 10)]') == [v1]
        assert read('//tmp/table[:(a, 11)]') == [v1, v2]
        assert read('//tmp/table[:]') == [v1, v2, v3, v4, v5]

        # combination of row and key selectors
        assert read('//tmp/table{i}[aa: (b, 10)]') == [{'i' : 5}]
        assert read('//tmp/table{a: o}[(b, 0): (c, 0)]') == \
            [
                {'i': 5, 'd' : 20.},
                {'i': 20,'d' : 20.},
                {'i': -100, 'd' : 10.}
            ]

        # limits of different types
        assert read('//tmp/table[#0:zz]') == [v1, v2, v3, v4, v5]


    def test_column_selector(self):
        create('table', '//tmp/table')

        write_str('//tmp/table', '{a = 1; aa = 2; b = 3; bb = 4; c = 5}')
        # empty columns
        assert read('//tmp/table{}') == [{}]

        # single columms
        assert read('//tmp/table{a}') == [{'a' : 1}]
        assert read('//tmp/table{a, }') == [{'a' : 1}] # extra comma
        assert read('//tmp/table{a, a}') == [{'a' : 1}]
        assert read('//tmp/table{c, b}') == [{'b' : 3, 'c' : 5}]
        assert read('//tmp/table{zzzzz}') == [{}] # non existent column

        # range columns
        # closed ranges
        with pytest.raises(YTError): read('//tmp/table{a:a}')  # left = right
        with pytest.raises(YTError): read('//tmp/table{b:a}')  # left > right

        assert read('//tmp/table{aa:b}') == [{'aa' : 2}]  # (+, +)
        assert read('//tmp/table{aa:bx}') == [{'aa' : 2, 'b' : 3, 'bb' : 4}]  # (+, -)
        assert read('//tmp/table{aaa:b}') == [{}]  # (-, +)
        assert read('//tmp/table{aaa:bx}') == [{'b' : 3, 'bb' : 4}] # (-, -)

        # open ranges
        # from left
        assert read('//tmp/table{:aa}') == [{'a' : 1}] # +
        assert read('//tmp/table{:aaa}') == [{'a' : 1, 'aa' : 2}] # -

        # from right
        assert read('//tmp/table{bb:}') == [{'bb' : 4, 'c' : 5}] # +
        assert read('//tmp/table{bz:}') == [{'c' : 5}] # -
        assert read('//tmp/table{xxx:}') == [{}]

        # fully open
        assert read('//tmp/table{:}') == [{'a' :1, 'aa': 2,  'b': 3, 'bb' : 4, 'c': 5}]

        # mixed column keys
        assert read('//tmp/table{aa, a:bb}') == [{'a' : 1, 'aa' : 2, 'b': 3}]

    def test_shared_locks_two_chunks(self):
        create('table', '//tmp/table')
        tx = start_transaction()

        write_str('<append=true>//tmp/table', '{a=1}', tx=tx)
        write_str('<append=true>//tmp/table', '{b=2}', tx=tx)

        assert read('//tmp/table') == []
        assert read('//tmp/table', tx=tx) == [{'a':1}, {'b':2}]

        commit_transaction(tx)
        assert read('//tmp/table') == [{'a':1}, {'b':2}]

    def test_shared_locks_three_chunks(self):
        create('table', '//tmp/table')
        tx = start_transaction()

        write_str('<append=true>//tmp/table', '{a=1}', tx=tx)
        write_str('<append=true>//tmp/table', '{b=2}', tx=tx)
        write_str('<append=true>//tmp/table', '{c=3}', tx=tx)

        assert read('//tmp/table') == []
        assert read('//tmp/table', tx=tx) == [{'a':1}, {'b':2}, {'c' : 3}]

        commit_transaction(tx)
        assert read('//tmp/table') == [{'a':1}, {'b':2}, {'c' : 3}]

    def test_shared_locks_parallel_tx(self):
        create('table', '//tmp/table')

        write_str('//tmp/table', '{a=1}')

        tx1 = start_transaction()
        tx2 = start_transaction()

        write_str('<append=true>//tmp/table', '{b=2}', tx=tx1)

        write_str('<append=true>//tmp/table', '{c=3}', tx=tx2)
        write_str('<append=true>//tmp/table', '{d=4}', tx=tx2)

        # check which records are seen from different transactions
        assert read('//tmp/table') == [{'a' : 1}]
        assert read('//tmp/table', tx = tx1) == [{'a' : 1}, {'b': 2}]
        assert read('//tmp/table', tx = tx2) == [{'a' : 1}, {'c': 3}, {'d' : 4}]

        commit_transaction(tx2)
        assert read('//tmp/table') == [{'a' : 1}, {'c': 3}, {'d' : 4}]
        assert read('//tmp/table', tx = tx1) == [{'a' : 1}, {'b': 2}]

        # now all records are in table in specific order
        commit_transaction(tx1)
        assert read('//tmp/table') == [{'a' : 1}, {'c': 3}, {'d' : 4}, {'b' : 2}]

    def test_shared_locks_nested_tx(self):
        create('table', '//tmp/table')

        v1 = {'k' : 1}
        v2 = {'k' : 2}
        v3 = {'k' : 3}
        v4 = {'k' : 4}

        outer_tx = start_transaction()

        write('//tmp/table', v1, tx=outer_tx)

        inner_tx = start_transaction(tx=outer_tx)

        write('<append=true>//tmp/table', v2, tx=inner_tx)
        assert read('//tmp/table', tx=outer_tx) == [v1]
        assert read('//tmp/table', tx=inner_tx) == [v1, v2]

        write('<append=true>//tmp/table', v3, tx=outer_tx) # this won't be seen from inner
        assert read('//tmp/table', tx=outer_tx) == [v1, v3]
        assert read('//tmp/table', tx=inner_tx) == [v1, v2]

        write('<append=true>//tmp/table', v4, tx=inner_tx)
        assert read('//tmp/table', tx=outer_tx) == [v1, v3]
        assert read('//tmp/table', tx=inner_tx) == [v1, v2, v4]

        commit_transaction(inner_tx)
        self.assertItemsEqual(read('//tmp/table', tx=outer_tx), [v1, v2, v4, v3]) # order is not specified

        commit_transaction(outer_tx)

    def test_codec_in_writer(self):
        create('table', '//tmp/table')
        set_str('//tmp/table/@compression_codec', "gzip_best_compression")
        write_str('//tmp/table', '{b="hello"}')

        assert read('//tmp/table') == [{"b":"hello"}]

        chunk_id = get("//tmp/table/@chunk_ids/0")
        assert get('#%s/@compression_codec' % chunk_id) == "gzip_best_compression"

    def test_copy(self):
        create('table', '//tmp/t')
        write_str('//tmp/t', '{a=b}')

        assert read('//tmp/t') == [{'a' : 'b'}]

        copy('//tmp/t', '//tmp/t2')
        assert read('//tmp/t2') == [{'a' : 'b'}]

        assert get('//tmp/t2/@resource_usage') == get('//tmp/t/@resource_usage')
        assert get('//tmp/t2/@replication_factor') == get('//tmp/t/@replication_factor')

        remove('//tmp/t')
        assert read('//tmp/t2') == [{'a' : 'b'}]

        remove('//tmp/t2')
        assert get_chunks() == []

    def test_copy_to_the_same_table(self):
        create('table', '//tmp/t')
        write_str('//tmp/t', '{a=b}')

        with pytest.raises(YTError): copy('//tmp/t', '//tmp/t')

    def test_copy_tx(self):
        create('table', '//tmp/t')
        write_str('//tmp/t', '{a=b}')

        tx = start_transaction()
        assert read('//tmp/t', tx=tx) == [{'a' : 'b'}]
        copy('//tmp/t', '//tmp/t2', tx=tx)
        assert read('//tmp/t2', tx=tx) == [{'a' : 'b'}]

        #assert get('//tmp/@recursive_resource_usage') == {"disk_space" : 438}
        #assert get('//tmp/@recursive_resource_usage', tx=tx) == {"disk_space" : 2 * 438}
        commit_transaction(tx)

        assert read('//tmp/t2') == [{'a' : 'b'}]

        remove('//tmp/t')
        assert read('//tmp/t2') == [{'a' : 'b'}]

        remove('//tmp/t2')
        assert get_chunks() == []

    def test_remove_create_under_transaction(self):
        create("table", "//tmp/table_xxx")
        tx = start_transaction()

        remove("//tmp/table_xxx", tx=tx)
        create("table", "//tmp/table_xxx", tx=tx)

    def test_transaction_staff(self):
        create("table", "//tmp/table_xxx")

        tx = start_transaction()
        remove("//tmp/table_xxx", tx=tx)
        inner_tx = start_transaction(tx=tx)
        get("//tmp", tx=inner_tx)

    def test_exists(self):
        self.assertEqual(exists("//tmp/t"), "false")
        self.assertEqual(exists("<overwrite=true>//tmp/t"), "false")

        create("table", "//tmp/t")
        self.assertEqual(exists("//tmp/t"), "true")
        self.assertEqual(exists("//tmp/t/x"), "false")
        self.assertEqual(exists("//tmp/t/1"), "false")
        self.assertEqual(exists("//tmp/t/1/t"), "false")
        self.assertEqual(exists("<overwrite=true>//tmp/t"), "true")
        # These cases are not supported yet because of future changes in the table grammar
        self.assertEqual(exists("//tmp/t[:#100]"), "true")
        #self.assertEqual(exists("//tmp/t/xxx"), "false")
        self.assertEqual(exists("//tmp/t/@"), "true")
        self.assertEqual(exists("//tmp/t/@chunk_ids"), "true")

    def test_invalid_channels_in_create(self):
        with pytest.raises(YTError): create('table', '//tmp/t', opt='channels=123')

    def test_replication_factor_attr(self):
        create('table', '//tmp/t')
        assert get('//tmp/t/@replication_factor') == 3

        with pytest.raises(YTError): remove('//tmp/t/@replication_factor')
        with pytest.raises(YTError): set('//tmp/t/@replication_factor', 0)
        with pytest.raises(YTError): set('//tmp/t/@replication_factor', {})

        tx = start_transaction()
        with pytest.raises(YTError): set('//tmp/t/@replication_factor', 2, tx=tx)

    def test_replication_factor_static(self):
        create('table', '//tmp/t')
        set('//tmp/t/@replication_factor', 2)

        write('//tmp/t', {'foo' : 'bar'})

        chunk_ids = get('//tmp/t/@chunk_ids')
        assert len(chunk_ids) == 1

        chunk_id = chunk_ids[0]
        assert get('#' + chunk_id + '/@replication_factor') == 2

    def test_recursive_resource_usage(self):
        create('table', '//tmp/t1')
        write_str('//tmp/t1', '{a=b}')
        copy('//tmp/t1', '//tmp/t2')

        assert get('//tmp/t1/@resource_usage')['disk_space'] + \
               get('//tmp/t2/@resource_usage')['disk_space'] == \
               get('//tmp/@recursive_resource_usage')['disk_space']

    def test_chunk_tree_balancer(self):
        create('table', '//tmp/t')
        for i in xrange(0, 40):
            write('<append=true>//tmp/t', {'a' : 'b'})
        chunk_list_id = get('//tmp/t/@chunk_list_id')
        statistics = get('#' + chunk_list_id + '/@statistics')
        assert statistics['chunk_count'] == 40
        assert statistics['chunk_list_count'] == 2
        assert statistics['row_count'] == 40
        assert statistics['rank'] == 2

    def _check_replication_factor(self, path, expected_rf):
        chunk_ids = get(path + '/@chunk_ids')
        for id in chunk_ids:
            assert get('#' + id + '/@replication_factor') == expected_rf

    def test_replication_factor_update1(self):
        create('table', '//tmp/t')
        for i in xrange(0, 5):
            write('<append=true>//tmp/t', {'a' : 'b'})
        set('//tmp/t/@replication_factor', 4)
        sleep(3)
        self._check_replication_factor('//tmp/t', 4)

    def test_replication_factor_update2(self):
        create('table', '//tmp/t')
        tx = start_transaction()
        for i in xrange(0, 5):
            write('<append=true>//tmp/t', {'a' : 'b'}, tx=tx)
        set('//tmp/t/@replication_factor', 4)
        commit_transaction(tx)
        sleep(3)
        self._check_replication_factor('//tmp/t', 4)

