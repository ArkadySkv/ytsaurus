import pytest

from yt_env_setup import YTEnvSetup
from yt_commands import *

import time


##################################################################

class TestTxCommands(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 0

    def test_simple(self):

        tx = start_transaction()
        
        #check that transaction is on the master (also within a tx)
        self.assertItemsEqual(get_transactions(), [tx])

        commit_transaction(tx)
        #check that transaction no longer exists
        self.assertItemsEqual(get_transactions(), [])

        #couldn't commit committed transaction
        with pytest.raises(YTError): commit_transaction(tx)
        #couldn't abort commited transaction
        with pytest.raises(YTError): abort_transaction(tx)

        ##############################################################
        #check the same for abort
        tx = start_transaction()

        self.assertItemsEqual(get_transactions(), [tx])
        
        abort_transaction(tx)
        #check that transaction no longer exists
        self.assertItemsEqual(get_transactions(), [])

        #couldn't commit aborted transaction
        with pytest.raises(YTError): commit_transaction(tx)
        #couldn't abort aborted transaction
        with pytest.raises(YTError): abort_transaction(tx)

    def test_changes_inside_tx(self):
        set('//tmp/value', '42')

        tx = start_transaction()
        set('//tmp/value', '100', tx = tx)

        # check that changes are not seen outside of transaction
        assert get('//tmp/value', tx = tx) == '100'
        assert get('//tmp/value') == '42'

        commit_transaction(tx)
        # changes after commit are applied
        assert get('//tmp/value') == '100'

        tx = start_transaction()
        set('//tmp/value', '100500', tx = tx)
        abort_transaction(tx)

        #changes after abort are not applied
        assert get('//tmp/value') == '100'

        remove('//tmp/value')

    def test_nested_tx(self):
        set('//tmp/t1', 0)

        tx_outer = start_transaction()

        tx1 = start_transaction(tx = tx_outer)
        set('//tmp/t1', 1, tx=tx1)

        tx2 = start_transaction(tx = tx_outer)

        # can't be committed as long there are uncommitted transactions
        with pytest.raises(YTError): commit_transaction(tx_outer)

        assert get('//tmp/t1', tx=tx_outer) == 0
        commit_transaction(tx1)
        assert get('//tmp/t1', tx=tx_outer) == 1

        # can be aborted..
        abort_transaction(tx_outer)
        
        # and this aborts all nested transactions
        assert get('//tmp/t1') == 0
        assert get_transactions() == []

    def test_timeout(self):
        tx = start_transaction(opt = '/timeout=4000')

        # check that transaction is still alive after 2 seconds
        time.sleep(2)
        self.assertItemsEqual(get_transactions(), [tx])

        # check that transaction is expired after 4 seconds
        time.sleep(2)
        self.assertItemsEqual(get_transactions(), [])

    def test_renew(self):
        tx = start_transaction(opt = '/timeout=4000')

        time.sleep(2)
        self.assertItemsEqual(get_transactions(), [tx])
        renew_transaction(tx)

        time.sleep(3)
        self.assertItemsEqual(get_transactions(), [tx])
        
        abort_transaction(tx)

    def test_expire_outer(self):
        tx_outer = start_transaction(opt = '/timeout=4000')
        tx_inner = start_transaction(tx = tx_outer)

        time.sleep(2)
        self.assertItemsEqual(get_transactions(), [tx_inner, tx_outer])
        renew_transaction(tx_inner)

        time.sleep(3)
        # check that outer tx expired (and therefore inner was aborted)
        self.assertItemsEqual(get_transactions(), [])

    def test_ping_ancestors(self):
        tx_outer = start_transaction(opt = '/timeout=4000')
        tx_inner = start_transaction(tx = tx_outer)

        time.sleep(2)
        self.assertItemsEqual(get_transactions(), [tx_inner, tx_outer])
        renew_transaction(tx_inner, '--ping_ancestor_txs')

        time.sleep(3)
        # check that all tx are still alive
        self.assertItemsEqual(get_transactions(), [tx_inner, tx_outer])

