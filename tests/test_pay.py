from fixtures import *  # noqa: F401,F403
from flaky import flaky  # noqa: F401
from lightning import RpcError, Millisatoshi
from utils import DEVELOPER, wait_for, only_one, sync_blockheight, SLOW_MACHINE


import copy
import concurrent.futures
import pytest
import random
import re
import string
import time
import unittest


def test_pay(node_factory):
    l1, l2 = node_factory.line_graph(2)

    inv = l2.rpc.invoice(123000, 'test_pay', 'description')['bolt11']
    before = int(time.time())
    details = l1.rpc.pay(inv)
    after = int(time.time())
    preimage = details['payment_preimage']
    assert details['status'] == 'complete'
    assert details['msatoshi'] == 123000
    assert details['destination'] == l2.info['id']
    assert details['created_at'] >= before
    assert details['created_at'] <= after

    invoices = l2.rpc.listinvoices('test_pay')['invoices']
    assert len(invoices) == 1
    invoice = invoices[0]
    assert invoice['status'] == 'paid' and invoice['paid_at'] >= before and invoice['paid_at'] <= after

    # Repeat payments are NOPs (if valid): we can hand null.
    l1.rpc.pay(inv)
    # This won't work: can't provide an amount (even if correct!)
    with pytest.raises(RpcError):
        l1.rpc.pay(inv, 123000)
    with pytest.raises(RpcError):
        l1.rpc.pay(inv, 122000)

    # Check pay_index is not null
    outputs = l2.db_query('SELECT pay_index IS NOT NULL AS q FROM invoices WHERE label="label";')
    assert len(outputs) == 1 and outputs[0]['q'] != 0

    # Check payment of any-amount invoice.
    for i in range(5):
        label = "any{}".format(i)
        inv2 = l2.rpc.invoice("any", label, 'description')['bolt11']
        # Must provide an amount!
        with pytest.raises(RpcError):
            l1.rpc.pay(inv2)
        l1.rpc.pay(inv2, random.randint(1000, 999999))

    # Should see 6 completed payments
    assert len(l1.rpc.listsendpays()['payments']) == 6

    # Test listsendpays indexed by bolt11.
    payments = l1.rpc.listsendpays(inv)['payments']
    assert len(payments) == 1 and payments[0]['payment_preimage'] == preimage


def test_pay_amounts(node_factory):
    l1, l2 = node_factory.line_graph(2)
    inv = l2.rpc.invoice(Millisatoshi("123sat"), 'test_pay_amounts', 'description')['bolt11']

    invoice = only_one(l2.rpc.listinvoices('test_pay_amounts')['invoices'])

    assert isinstance(invoice['amount_msat'], Millisatoshi)
    assert invoice['amount_msat'] == Millisatoshi(123000)

    l1.rpc.pay(inv)

    invoice = only_one(l2.rpc.listinvoices('test_pay_amounts')['invoices'])
    assert isinstance(invoice['amount_received_msat'], Millisatoshi)
    assert invoice['amount_received_msat'] >= Millisatoshi(123000)


def test_pay_limits(node_factory):
    """Test that we enforce fee max percentage and max delay"""
    l1, l2, l3 = node_factory.line_graph(3, wait_for_announce=True)

    # FIXME: pylightning should define these!
    PAY_ROUTE_TOO_EXPENSIVE = 206

    inv = l3.rpc.invoice("any", "any", 'description')

    # Fee too high.
    with pytest.raises(RpcError, match=r'Route wanted fee of .*msat') as err:
        l1.rpc.call('pay', {'bolt11': inv['bolt11'], 'msatoshi': 100000, 'maxfeepercent': 0.0001, 'exemptfee': 0})

    assert err.value.error['code'] == PAY_ROUTE_TOO_EXPENSIVE

    # It should have retried (once without routehint, too)
    status = l1.rpc.call('paystatus', {'bolt11': inv['bolt11']})['pay'][0]['attempts']

    # Hits weird corner case: it excludes channel, then uses routehint
    # which reintroduces it, so then it excludes other channel.
    assert len(status) == 4
    assert status[0]['strategy'] == "Initial attempt"
    assert status[1]['strategy'].startswith("Excluded expensive channel ")
    assert status[2]['strategy'] == "Trying route hint"
    assert status[3]['strategy'].startswith("Excluded expensive channel ")

    # Delay too high.
    with pytest.raises(RpcError, match=r'Route wanted delay of .* blocks') as err:
        l1.rpc.call('pay', {'bolt11': inv['bolt11'], 'msatoshi': 100000, 'maxdelay': 0})

    assert err.value.error['code'] == PAY_ROUTE_TOO_EXPENSIVE
    # Should also have retried.
    status = l1.rpc.call('paystatus', {'bolt11': inv['bolt11']})['pay'][1]['attempts']
    assert len(status) == 4
    assert status[0]['strategy'] == "Initial attempt"
    assert status[1]['strategy'].startswith("Excluded delaying channel ")
    assert status[2]['strategy'] == "Trying route hint"
    assert status[3]['strategy'].startswith("Excluded delaying channel ")

    # This works, because fee is less than exemptfee.
    l1.rpc.call('pay', {'bolt11': inv['bolt11'], 'msatoshi': 100000, 'maxfeepercent': 0.0001, 'exemptfee': 2000})
    status = l1.rpc.call('paystatus', {'bolt11': inv['bolt11']})['pay'][2]['attempts']
    assert len(status) == 1
    assert status[0]['strategy'] == "Initial attempt"


def test_pay0(node_factory):
    """Test paying 0 amount
    """
    l1, l2 = node_factory.line_graph(2)
    chanid = l1.get_channel_scid(l2)

    # Get any-amount invoice
    inv = l2.rpc.invoice("any", "any", 'description')
    rhash = inv['payment_hash']

    routestep = {
        'msatoshi': 0,
        'id': l2.info['id'],
        'delay': 10,
        'channel': chanid
    }

    # Amount must be nonzero!
    l1.rpc.sendpay([routestep], rhash)
    with pytest.raises(RpcError, match=r'WIRE_AMOUNT_BELOW_MINIMUM'):
        l1.rpc.waitsendpay(rhash)


@unittest.skipIf(not DEVELOPER, "needs DEVELOPER=1")
def test_pay_disconnect(node_factory, bitcoind):
    """If the remote node has disconnected, we fail payment, but can try again when it reconnects"""
    l1, l2 = node_factory.line_graph(2, opts={'dev-max-fee-multiplier': 5,
                                              'may_reconnect': True})

    inv = l2.rpc.invoice(123000, 'test_pay_disconnect', 'description')
    rhash = inv['payment_hash']

    wait_for(lambda: [c['active'] for c in l1.rpc.listchannels()['channels']] == [True, True])

    # Can't use `pay` since that'd notice that we can't route, due to disabling channel_update
    route = l1.rpc.getroute(l2.info['id'], 123000, 1)["route"]

    l2.stop()
    wait_for(lambda: [c['active'] for c in l1.rpc.listchannels()['channels']] == [False, False])

    # Can't pay while its offline.
    with pytest.raises(RpcError, match=r'failed: WIRE_TEMPORARY_CHANNEL_FAILURE \(First peer not ready\)'):
        l1.rpc.sendpay(route, rhash)

    l2.start()
    l1.daemon.wait_for_log('peer_out WIRE_CHANNEL_REESTABLISH')

    # Make l2 upset by asking for crazy fee.
    l1.set_feerates((10**6, 1000**6, 1000**6), False)

    # Wait for l1 notice
    l1.daemon.wait_for_log(r'Peer permanent failure in CHANNELD_NORMAL: lightning_channeld: received ERROR channel .*: update_fee \d+ outside range 1875-75000')

    # Should fail due to permenant channel fail
    with pytest.raises(RpcError, match=r'failed: WIRE_UNKNOWN_NEXT_PEER \(First peer not ready\)'):
        l1.rpc.sendpay(route, rhash)

    assert not l1.daemon.is_in_log('Payment is still in progress')

    # After it sees block, someone should close channel.
    bitcoind.generate_block(1)
    l1.daemon.wait_for_log('ONCHAIN')


@unittest.skipIf(not DEVELOPER, "needs DEVELOPER=1 for dev_suppress_gossip")
def test_pay_get_error_with_update(node_factory):
    """We should process an update inside a temporary_channel_failure"""
    l1, l2, l3 = node_factory.line_graph(3, opts={'log-level': 'io'}, fundchannel=True, wait_for_announce=True)
    chanid2 = l2.get_channel_scid(l3)

    inv = l3.rpc.invoice(123000, 'test_pay_get_error_with_update', 'description')

    route = l1.rpc.getroute(l3.info['id'], 12300, 1)["route"]

    # Make sure l2 doesn't tell l1 directly that channel is disabled.
    l2.rpc.dev_suppress_gossip()
    l3.stop()

    # Make sure that l2 has seen disconnect, considers channel disabled.
    wait_for(lambda: [c['active'] for c in l2.rpc.listchannels(chanid2)['channels']] == [False, False])

    l1.rpc.sendpay(route, inv['payment_hash'])
    with pytest.raises(RpcError, match=r'WIRE_TEMPORARY_CHANNEL_FAILURE'):
        l1.rpc.waitsendpay(inv['payment_hash'])

    # Make sure we get an onionreply, without the type prefix of the nested
    # channel_update, and it should patch it to include a type prefix. The
    # prefix 0x0102 should be in the channel_update, but not in the
    # onionreply (negation of 0x0102 in the RE)
    l1.daemon.wait_for_log(r'Extracted channel_update 0102.*from onionreply 10070088[0-9a-fA-F]{88}')

    # And now monitor for l1 to apply the channel_update we just extracted
    l1.daemon.wait_for_log(r'Received channel_update for channel {}/. now DISABLED \(from error\)'.format(chanid2))


def test_pay_optional_args(node_factory):
    l1, l2 = node_factory.line_graph(2)

    inv1 = l2.rpc.invoice(123000, 'test_pay', 'desc')['bolt11']
    l1.rpc.pay(inv1, label='desc')
    payment1 = l1.rpc.listsendpays(inv1)['payments']
    assert len(payment1) and payment1[0]['msatoshi'] == 123000
    assert payment1[0]['label'] == 'desc'

    inv2 = l2.rpc.invoice(321000, 'test_pay2', 'description')['bolt11']
    l1.rpc.pay(inv2, riskfactor=5.0)
    payment2 = l1.rpc.listsendpays(inv2)['payments']
    assert len(payment2) == 1 and payment2[0]['msatoshi'] == 321000

    anyinv = l2.rpc.invoice('any', 'any_pay', 'desc')['bolt11']
    l1.rpc.pay(anyinv, label='desc', msatoshi='500')
    payment3 = l1.rpc.listsendpays(anyinv)['payments']
    assert len(payment3) == 1 and payment3[0]['msatoshi'] == 500
    assert payment3[0]['label'] == 'desc'

    # Should see 3 completed transactions
    assert len(l1.rpc.listsendpays()['payments']) == 3


@unittest.skipIf(not DEVELOPER, "needs DEVELOPER=1")
def test_payment_success_persistence(node_factory, bitcoind, executor):
    # Start two nodes and open a channel.. die during payment.
    # Feerates identical so we don't get gratuitous commit to update them
    l1 = node_factory.get_node(disconnect=['+WIRE_COMMITMENT_SIGNED'],
                               options={'dev-no-reconnect': None},
                               may_reconnect=True,
                               feerates=(7500, 7500, 7500))
    l2 = node_factory.get_node(may_reconnect=True)
    l1.rpc.connect(l2.info['id'], 'localhost', l2.port)

    chanid = l1.fund_channel(l2, 100000)

    inv1 = l2.rpc.invoice(1000, 'inv1', 'inv1')

    # Fire off a pay request, it'll get interrupted by a restart
    executor.submit(l1.rpc.pay, inv1['bolt11'])

    l1.daemon.wait_for_log(r'dev_disconnect: \+WIRE_COMMITMENT_SIGNED')

    print("Killing l1 in mid HTLC")
    l1.daemon.kill()

    # Restart l1, without disconnect stuff.
    del l1.daemon.opts['dev-no-reconnect']
    del l1.daemon.opts['dev-disconnect']

    # Should reconnect, and sort the payment out.
    l1.start()

    wait_for(lambda: l1.rpc.listsendpays()['payments'][0]['status'] != 'pending')

    payments = l1.rpc.listsendpays()['payments']
    invoices = l2.rpc.listinvoices('inv1')['invoices']
    assert len(payments) == 1 and payments[0]['status'] == 'complete'
    assert len(invoices) == 1 and invoices[0]['status'] == 'paid'

    l1.wait_channel_active(chanid)

    # A duplicate should succeed immediately (nop) and return correct preimage.
    preimage = l1.rpc.pay(inv1['bolt11'])['payment_preimage']
    assert l1.rpc.dev_rhash(preimage)['rhash'] == inv1['payment_hash']


@unittest.skipIf(not DEVELOPER, "needs DEVELOPER=1")
def test_payment_failed_persistence(node_factory, executor):
    # Start two nodes and open a channel.. die during payment.
    # Feerates identical so we don't get gratuitous commit to update them
    l1 = node_factory.get_node(disconnect=['+WIRE_COMMITMENT_SIGNED'],
                               options={'dev-no-reconnect': None},
                               may_reconnect=True,
                               feerates=(7500, 7500, 7500))
    l2 = node_factory.get_node(may_reconnect=True)
    l1.rpc.connect(l2.info['id'], 'localhost', l2.port)

    l1.fund_channel(l2, 100000)

    # Expires almost immediately, so it will fail.
    inv1 = l2.rpc.invoice(1000, 'inv1', 'inv1', 5)

    # Fire off a pay request, it'll get interrupted by a restart
    executor.submit(l1.rpc.pay, inv1['bolt11'])

    l1.daemon.wait_for_log(r'dev_disconnect: \+WIRE_COMMITMENT_SIGNED')

    print("Killing l1 in mid HTLC")
    l1.daemon.kill()

    # Restart l1, without disconnect stuff.
    del l1.daemon.opts['dev-no-reconnect']
    del l1.daemon.opts['dev-disconnect']

    # Make sure invoice has expired.
    time.sleep(5 + 1)

    # Should reconnect, and fail the payment
    l1.start()

    wait_for(lambda: l1.rpc.listsendpays()['payments'][0]['status'] != 'pending')

    payments = l1.rpc.listsendpays()['payments']
    invoices = l2.rpc.listinvoices('inv1')['invoices']
    assert len(invoices) == 1 and invoices[0]['status'] == 'expired'
    assert len(payments) == 1 and payments[0]['status'] == 'failed'

    # Another attempt should also fail.
    with pytest.raises(RpcError):
        l1.rpc.pay(inv1['bolt11'])


@unittest.skipIf(not DEVELOPER, "needs DEVELOPER=1")
def test_payment_duplicate_uncommitted(node_factory, executor):
    # We want to test two payments at the same time, before we send commit
    l1 = node_factory.get_node(disconnect=['=WIRE_UPDATE_ADD_HTLC-nocommit'])
    l2 = node_factory.get_node()

    l1.rpc.connect(l2.info['id'], 'localhost', l2.port)

    l1.fund_channel(l2, 100000)

    inv1 = l2.rpc.invoice(1000, 'inv1', 'inv1')

    # Start first payment, but not yet in db.
    fut = executor.submit(l1.rpc.pay, inv1['bolt11'])

    # Make sure that's started...
    l1.daemon.wait_for_log('dev_disconnect: =WIRE_UPDATE_ADD_HTLC-nocommit')

    # We should see it in listsendpays
    payments = l1.rpc.listsendpays()['payments']
    assert len(payments) == 1
    assert payments[0]['status'] == 'pending' and payments[0]['payment_hash'] == inv1['payment_hash']

    # Second one will succeed eventually.
    fut2 = executor.submit(l1.rpc.pay, inv1['bolt11'])

    # Now, let it commit.
    l1.rpc.dev_reenable_commit(l2.info['id'])

    # These should succeed.
    fut.result(10)
    fut2.result(10)


def test_sendpay(node_factory):
    l1, l2 = node_factory.line_graph(2, fundamount=10**6)

    amt = 200000000
    rhash = l2.rpc.invoice(amt, 'testpayment2', 'desc')['payment_hash']

    def invoice_unpaid(dst, label):
        invoices = dst.rpc.listinvoices(label)['invoices']
        return len(invoices) == 1 and invoices[0]['status'] == 'unpaid'

    def only_one(arr):
        assert len(arr) == 1
        return arr[0]

    routestep = {
        'msatoshi': amt,
        'id': l2.info['id'],
        'delay': 5,
        'channel': '1x1x1'
    }

    # Insufficient funds.
    with pytest.raises(RpcError):
        rs = copy.deepcopy(routestep)
        rs['msatoshi'] = rs['msatoshi'] - 1
        l1.rpc.sendpay([rs], rhash)
        l1.rpc.waitsendpay(rhash)
    assert invoice_unpaid(l2, 'testpayment2')

    # Gross overpayment (more than factor of 2)
    with pytest.raises(RpcError):
        rs = copy.deepcopy(routestep)
        rs['msatoshi'] = rs['msatoshi'] * 2 + 1
        l1.rpc.sendpay([rs], rhash)
        l1.rpc.waitsendpay(rhash)
    assert invoice_unpaid(l2, 'testpayment2')

    # Insufficient delay.
    with pytest.raises(RpcError):
        rs = copy.deepcopy(routestep)
        rs['delay'] = rs['delay'] - 2
        l1.rpc.sendpay([rs], rhash)
        l1.rpc.waitsendpay(rhash)
    assert invoice_unpaid(l2, 'testpayment2')

    # Bad ID.
    with pytest.raises(RpcError):
        rs = copy.deepcopy(routestep)
        rs['id'] = '00000000000000000000000000000000'
        l1.rpc.sendpay([rs], rhash)
    assert invoice_unpaid(l2, 'testpayment2')

    # FIXME: test paying via another node, should fail to pay twice.
    p1 = l1.rpc.getpeer(l2.info['id'], 'info')
    p2 = l2.rpc.getpeer(l1.info['id'], 'info')
    assert only_one(p1['channels'])['msatoshi_to_us'] == 10**6 * 1000
    assert only_one(p1['channels'])['msatoshi_total'] == 10**6 * 1000
    assert only_one(p2['channels'])['msatoshi_to_us'] == 0
    assert only_one(p2['channels'])['msatoshi_total'] == 10**6 * 1000

    # This works.
    before = int(time.time())
    details = l1.rpc.sendpay([routestep], rhash)
    after = int(time.time())
    preimage = l1.rpc.waitsendpay(rhash)['payment_preimage']
    # Check details
    assert details['payment_hash'] == rhash
    assert details['destination'] == l2.info['id']
    assert details['msatoshi'] == amt
    assert details['created_at'] >= before
    assert details['created_at'] <= after
    # Check receiver
    assert only_one(l2.rpc.listinvoices('testpayment2')['invoices'])['status'] == 'paid'
    assert only_one(l2.rpc.listinvoices('testpayment2')['invoices'])['pay_index'] == 1
    assert only_one(l2.rpc.listinvoices('testpayment2')['invoices'])['msatoshi_received'] == rs['msatoshi']

    # Balances should reflect it.
    def check_balances():
        p1 = l1.rpc.getpeer(l2.info['id'], 'info')
        p2 = l2.rpc.getpeer(l1.info['id'], 'info')
        return (
            only_one(p1['channels'])['msatoshi_to_us'] == 10**6 * 1000 - amt
            and only_one(p1['channels'])['msatoshi_total'] == 10**6 * 1000
            and only_one(p2['channels'])['msatoshi_to_us'] == amt
            and only_one(p2['channels'])['msatoshi_total'] == 10**6 * 1000
        )
    wait_for(check_balances)

    # Repeat will "succeed", but won't actually send anything (duplicate)
    assert not l1.daemon.is_in_log('... succeeded')
    details = l1.rpc.sendpay([routestep], rhash)
    assert details['status'] == "complete"
    preimage2 = details['payment_preimage']
    assert preimage == preimage2
    l1.daemon.wait_for_log('... succeeded')
    assert only_one(l2.rpc.listinvoices('testpayment2')['invoices'])['status'] == 'paid'
    assert only_one(l2.rpc.listinvoices('testpayment2')['invoices'])['msatoshi_received'] == rs['msatoshi']

    # Overpaying by "only" a factor of 2 succeeds.
    rhash = l2.rpc.invoice(amt, 'testpayment3', 'desc')['payment_hash']
    assert only_one(l2.rpc.listinvoices('testpayment3')['invoices'])['status'] == 'unpaid'
    routestep = {'msatoshi': amt * 2, 'id': l2.info['id'], 'delay': 5, 'channel': '1x1x1'}
    l1.rpc.sendpay([routestep], rhash)
    preimage3 = l1.rpc.waitsendpay(rhash)['payment_preimage']
    assert only_one(l2.rpc.listinvoices('testpayment3')['invoices'])['status'] == 'paid'
    assert only_one(l2.rpc.listinvoices('testpayment3')['invoices'])['msatoshi_received'] == amt * 2

    # Test listsendpays
    payments = l1.rpc.listsendpays()['payments']
    assert len(payments) == 2

    invoice2 = only_one(l2.rpc.listinvoices('testpayment2')['invoices'])
    payments = l1.rpc.listsendpays(payment_hash=invoice2['payment_hash'])['payments']
    assert len(payments) == 1

    assert payments[0]['status'] == 'complete'
    assert payments[0]['payment_preimage'] == preimage2

    invoice3 = only_one(l2.rpc.listinvoices('testpayment3')['invoices'])
    payments = l1.rpc.listsendpays(payment_hash=invoice3['payment_hash'])['payments']
    assert len(payments) == 1

    assert payments[0]['status'] == 'complete'
    assert payments[0]['payment_preimage'] == preimage3


def test_sendpay_cant_afford(node_factory):
    l1, l2 = node_factory.line_graph(2, fundamount=10**6)

    # Can't pay more than channel capacity.
    def pay(lsrc, ldst, amt, label=None):
        if not label:
            label = ''.join(random.choice(string.ascii_letters + string.digits) for _ in range(20))
        rhash = ldst.rpc.invoice(amt, label, label)['payment_hash']
        routestep = {'msatoshi': amt, 'id': ldst.info['id'], 'delay': 5, 'channel': '1x1x1'}
        lsrc.rpc.sendpay([routestep], rhash)
        lsrc.rpc.waitsendpay(rhash)

    with pytest.raises(RpcError):
        pay(l1, l2, 10**9 + 1)

    # This is the fee, which needs to be taken into account for l1.
    available = 10**9 - 13440000
    # Reserve is 1%.
    reserve = 10**7

    # Can't pay past reserve.
    with pytest.raises(RpcError):
        pay(l1, l2, available)
    with pytest.raises(RpcError):
        pay(l1, l2, available - reserve + 1)

    # Can pay up to reserve (1%)
    pay(l1, l2, available - reserve)

    # And now it can't pay back, due to its own reserve.
    with pytest.raises(RpcError):
        pay(l2, l1, available - reserve)

    # But this should work.
    pay(l2, l1, available - reserve * 2)


def test_decodepay(node_factory):
    l1 = node_factory.get_node()

    # BOLT #11:
    # > ### Please make a donation of any amount using payment_hash 0001020304050607080900010203040506070809000102030405060708090102 to me @03e7156ae33b0a208d0744199163177e909e80176e55d97a2f221ede0f934dd9ad
    # > lnbc1pvjluezpp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqdpl2pkx2ctnv5sxxmmwwd5kgetjypeh2ursdae8g6twvus8g6rfwvs8qun0dfjkxaq8rkx3yf5tcsyz3d73gafnh3cax9rn449d9p5uxz9ezhhypd0elx87sjle52x86fux2ypatgddc6k63n7erqz25le42c4u4ecky03ylcqca784w
    #
    # Breakdown:
    #
    # * `lnbc`: prefix, lightning on bitcoin mainnet
    # * `1`: Bech32 separator
    # * `pvjluez`: timestamp (1496314658)
    # * `p`: payment hash
    #   * `p5`: `data_length` (`p` = 1, `5` = 20. 1 * 32 + 20 == 52)
    #   * `qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypq`: payment hash 0001020304050607080900010203040506070809000102030405060708090102
    # * `d`: short description
    #   * `pl`: `data_length` (`p` = 1, `l` = 31. 1 * 32 + 31 == 63)
    #   * `2pkx2ctnv5sxxmmwwd5kgetjypeh2ursdae8g6twvus8g6rfwvs8qun0dfjkxaq`: 'Please consider supporting this project'
    # * `32vjcgqxyuj7nqphl3xmmhls2rkl3t97uan4j0xa87gj5779czc8p0z58zf5wpt9ggem6adl64cvawcxlef9djqwp2jzzfvs272504sp`: signature
    # * `0lkg3c`: Bech32 checksum
    b11 = l1.rpc.decodepay(
        'lnbc1pvjluezpp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqd'
        'pl2pkx2ctnv5sxxmmwwd5kgetjypeh2ursdae8g6twvus8g6rfwvs8qun0dfjkxaq8rk'
        'x3yf5tcsyz3d73gafnh3cax9rn449d9p5uxz9ezhhypd0elx87sjle52x86fux2ypatg'
        'ddc6k63n7erqz25le42c4u4ecky03ylcqca784w'
    )
    assert b11['currency'] == 'bc'
    assert b11['created_at'] == 1496314658
    assert b11['payment_hash'] == '0001020304050607080900010203040506070809000102030405060708090102'
    assert b11['description'] == 'Please consider supporting this project'
    assert b11['expiry'] == 3600
    assert b11['payee'] == '03e7156ae33b0a208d0744199163177e909e80176e55d97a2f221ede0f934dd9ad'

    # BOLT #11:
    # > ### Please send $3 for a cup of coffee to the same peer, within 1 minute
    # > lnbc2500u1pvjluezpp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqdq5xysxxatsyp3k7enxv4jsxqzpuaztrnwngzn3kdzw5hydlzf03qdgm2hdq27cqv3agm2awhz5se903vruatfhq77w3ls4evs3ch9zw97j25emudupq63nyw24cg27h2rspfj9srp
    #
    # Breakdown:
    #
    # * `lnbc`: prefix, lightning on bitcoin mainnet
    # * `2500u`: amount (2500 micro-bitcoin)
    # * `1`: Bech32 separator
    # * `pvjluez`: timestamp (1496314658)
    # * `p`: payment hash...
    # * `d`: short description
    #   * `q5`: `data_length` (`q` = 0, `5` = 20. 0 * 32 + 20 == 20)
    #   * `xysxxatsyp3k7enxv4js`: '1 cup coffee'
    # * `x`: expiry time
    #   * `qz`: `data_length` (`q` = 0, `z` = 2. 0 * 32 + 2 == 2)
    #   * `pu`: 60 seconds (`p` = 1, `u` = 28.  1 * 32 + 28 == 60)
    # * `azh8qt5w7qeewkmxtv55khqxvdfs9zzradsvj7rcej9knpzdwjykcq8gv4v2dl705pjadhpsc967zhzdpuwn5qzjm0s4hqm2u0vuhhqq`: signature
    # * `7vc09u`: Bech32 checksum
    b11 = l1.rpc.decodepay(
        'lnbc2500u1pvjluezpp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqf'
        'qypqdq5xysxxatsyp3k7enxv4jsxqzpuaztrnwngzn3kdzw5hydlzf03qdgm2hdq27cq'
        'v3agm2awhz5se903vruatfhq77w3ls4evs3ch9zw97j25emudupq63nyw24cg27h2rsp'
        'fj9srp'
    )
    assert b11['currency'] == 'bc'
    assert b11['msatoshi'] == 2500 * 10**11 // 1000000
    assert b11['amount_msat'] == Millisatoshi(2500 * 10**11 // 1000000)
    assert b11['created_at'] == 1496314658
    assert b11['payment_hash'] == '0001020304050607080900010203040506070809000102030405060708090102'
    assert b11['description'] == '1 cup coffee'
    assert b11['expiry'] == 60
    assert b11['payee'] == '03e7156ae33b0a208d0744199163177e909e80176e55d97a2f221ede0f934dd9ad'

    # BOLT #11:
    # > ### Now send $24 for an entire list of things (hashed)
    # > lnbc20m1pvjluezpp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqscc6gd6ql3jrc5yzme8v4ntcewwz5cnw92tz0pc8qcuufvq7khhr8wpald05e92xw006sq94mg8v2ndf4sefvf9sygkshp5zfem29trqq2yxxz7
    #
    # Breakdown:
    #
    # * `lnbc`: prefix, lightning on bitcoin mainnet
    # * `20m`: amount (20 milli-bitcoin)
    # * `1`: Bech32 separator
    # * `pvjluez`: timestamp (1496314658)
    # * `p`: payment hash...
    # * `h`: tagged field: hash of description
    # * `p5`: `data_length` (`p` = 1, `5` = 20. 1 * 32 + 20 == 52)
    # * `8yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqs`: SHA256 of 'One piece of chocolate cake, one icecream cone, one pickle, one slice of swiss cheese, one slice of salami, one lollypop, one piece of cherry pie, one sausage, one cupcake, and one slice of watermelon'
    # * `vjfls3ljx9e93jkw0kw40yxn4pevgzflf83qh2852esjddv4xk4z70nehrdcxa4fk0t6hlcc6vrxywke6njenk7yzkzw0quqcwxphkcp`: signature
    # * `vam37w`: Bech32 checksum
    b11 = l1.rpc.decodepay(
        'lnbc20m1pvjluezpp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqy'
        'pqhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqscc6gd6ql3jr'
        'c5yzme8v4ntcewwz5cnw92tz0pc8qcuufvq7khhr8wpald05e92xw006sq94mg8v2ndf'
        '4sefvf9sygkshp5zfem29trqq2yxxz7',
        'One piece of chocolate cake, one icecream cone, one pickle, one slic'
        'e of swiss cheese, one slice of salami, one lollypop, one piece of c'
        'herry pie, one sausage, one cupcake, and one slice of watermelon'
    )
    assert b11['currency'] == 'bc'
    assert b11['msatoshi'] == 20 * 10**11 // 1000
    assert b11['amount_msat'] == Millisatoshi(str(20 * 10**11 // 1000) + 'msat')
    assert b11['created_at'] == 1496314658
    assert b11['payment_hash'] == '0001020304050607080900010203040506070809000102030405060708090102'
    assert b11['expiry'] == 3600
    assert b11['payee'] == '03e7156ae33b0a208d0744199163177e909e80176e55d97a2f221ede0f934dd9ad'

    # > ### The same, on testnet, with a fallback address mk2QpYatsKicvFVuTAQLBryyccRXMUaGHP
    # > lntb20m1pvjluezhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqspp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqfpp3x9et2e20v6pu37c5d9vax37wxq72un98kmzzhznpurw9sgl2v0nklu2g4d0keph5t7tj9tcqd8rexnd07ux4uv2cjvcqwaxgj7v4uwn5wmypjd5n69z2xm3xgksg28nwht7f6zspwp3f9t
    #
    # Breakdown:
    #
    # * `lntb`: prefix, lightning on bitcoin testnet
    # * `20m`: amount (20 milli-bitcoin)
    # * `1`: Bech32 separator
    # * `pvjluez`: timestamp (1496314658)
    # * `p`: payment hash...
    # * `f`: tagged field: fallback address
    # * `pp`: `data_length` (`p` = 1. 1 * 32 + 1 == 33)
    # * `3x9et2e20v6pu37c5d9vax37wxq72un98`: `3` = 17, so P2PKH address
    # * `h`: tagged field: hash of description...
    # * `qh84fmvn2klvglsjxfy0vq2mz6t9kjfzlxfwgljj35w2kwa60qv49k7jlsgx43yhs9nuutllkhhnt090mmenuhp8ue33pv4klmrzlcqp`: signature
    # * `us2s2r`: Bech32 checksum
    b11 = l1.rpc.decodepay(
        'lntb20m1pvjluezhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahr'
        'qspp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqfpp3x9et2e2'
        '0v6pu37c5d9vax37wxq72un98kmzzhznpurw9sgl2v0nklu2g4d0keph5t7tj9tcqd8r'
        'exnd07ux4uv2cjvcqwaxgj7v4uwn5wmypjd5n69z2xm3xgksg28nwht7f6zspwp3f9t',
        'One piece of chocolate cake, one icecream cone, one pickle, one slic'
        'e of swiss cheese, one slice of salami, one lollypop, one piece of c'
        'herry pie, one sausage, one cupcake, and one slice of watermelon'
    )
    assert b11['currency'] == 'tb'
    assert b11['msatoshi'] == 20 * 10**11 // 1000
    assert b11['amount_msat'] == Millisatoshi(20 * 10**11 // 1000)
    assert b11['created_at'] == 1496314658
    assert b11['payment_hash'] == '0001020304050607080900010203040506070809000102030405060708090102'
    assert b11['expiry'] == 3600
    assert b11['payee'] == '03e7156ae33b0a208d0744199163177e909e80176e55d97a2f221ede0f934dd9ad'
    assert len(b11['fallbacks']) == 1
    assert b11['fallbacks'][0]['type'] == 'P2PKH'
    assert b11['fallbacks'][0]['addr'] == 'mk2QpYatsKicvFVuTAQLBryyccRXMUaGHP'

    # > ### On mainnet, with fallback address 1RustyRX2oai4EYYDpQGWvEL62BBGqN9T with extra routing info to go via nodes 029e03a901b85534ff1e92c43c74431f7ce72046060fcf7a95c37e148f78c77255 then 039e03a901b85534ff1e92c43c74431f7ce72046060fcf7a95c37e148f78c77255
    # > lnbc20m1pvjluezpp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqsfpp3qjmp7lwpagxun9pygexvgpjdc4jdj85fr9yq20q82gphp2nflc7jtzrcazrra7wwgzxqc8u7754cdlpfrmccae92qgzqvzq2ps8pqqqqqqpqqqqq9qqqvpeuqafqxu92d8lr6fvg0r5gv0heeeqgcrqlnm6jhphu9y00rrhy4grqszsvpcgpy9qqqqqqgqqqqq7qqzqj9n4evl6mr5aj9f58zp6fyjzup6ywn3x6sk8akg5v4tgn2q8g4fhx05wf6juaxu9760yp46454gpg5mtzgerlzezqcqvjnhjh8z3g2qqdhhwkj
    #
    # Breakdown:
    #
    # * `lnbc`: prefix, lightning on bitcoin mainnet
    # * `20m`: amount (20 milli-bitcoin)
    # * `1`: Bech32 separator
    # * `pvjluez`: timestamp (1496314658)
    # * `p`: payment hash...
    # * `h`: tagged field: hash of description...
    # * `f`: tagged field: fallback address
    #   * `pp`: `data_length` (`p` = 1. 1 * 32 + 1 == 33)
    #   * `3` = 17, so P2PKH address
    #   * `qjmp7lwpagxun9pygexvgpjdc4jdj85f`: 160 bit P2PKH address
    # * `r`: tagged field: route information
    #   * `9y`: `data_length` (`9` = 5, `y` = 4.  5 * 32 + 4 = 164)
    #     `q20q82gphp2nflc7jtzrcazrra7wwgzxqc8u7754cdlpfrmccae92qgzqvzq2ps8pqqqqqqqqqqqq9qqqvpeuqafqxu92d8lr6fvg0r5gv0heeeqgcrqlnm6jhphu9y00rrhy4grqszsvpcgpy9qqqqqqqqqqqq7qqzq`: pubkey `029e03a901b85534ff1e92c43c74431f7ce72046060fcf7a95c37e148f78c77255`, `short_channel_id` 0102030405060708, `fee_base_msat` 1 millisatoshi, `fee_proportional_millionths` 20, `cltv_expiry_delta` 3.  pubkey `039e03a901b85534ff1e92c43c74431f7ce72046060fcf7a95c37e148f78c77255`, `short_channel_id` 030405060708090a, `fee_base_msat` 2 millisatoshi, `fee_proportional_millionths` 30, `cltv_expiry_delta` 4.
    # * `j9n4evl6mr5aj9f58zp6fyjzup6ywn3x6sk8akg5v4tgn2q8g4fhx05wf6juaxu9760yp46454gpg5mtzgerlzezqcqvjnhjh8z3g2qq`: signature
    # * `dhhwkj`: Bech32 checksum
    b11 = l1.rpc.decodepay('lnbc20m1pvjluezpp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqsfpp3qjmp7lwpagxun9pygexvgpjdc4jdj85fr9yq20q82gphp2nflc7jtzrcazrra7wwgzxqc8u7754cdlpfrmccae92qgzqvzq2ps8pqqqqqqpqqqqq9qqqvpeuqafqxu92d8lr6fvg0r5gv0heeeqgcrqlnm6jhphu9y00rrhy4grqszsvpcgpy9qqqqqqgqqqqq7qqzqj9n4evl6mr5aj9f58zp6fyjzup6ywn3x6sk8akg5v4tgn2q8g4fhx05wf6juaxu9760yp46454gpg5mtzgerlzezqcqvjnhjh8z3g2qqdhhwkj', 'One piece of chocolate cake, one icecream cone, one pickle, one slice of swiss cheese, one slice of salami, one lollypop, one piece of cherry pie, one sausage, one cupcake, and one slice of watermelon')
    assert b11['currency'] == 'bc'
    assert b11['msatoshi'] == 20 * 10**11 // 1000
    assert b11['amount_msat'] == Millisatoshi(20 * 10**11 // 1000)
    assert b11['created_at'] == 1496314658
    assert b11['payment_hash'] == '0001020304050607080900010203040506070809000102030405060708090102'
    assert b11['expiry'] == 3600
    assert b11['payee'] == '03e7156ae33b0a208d0744199163177e909e80176e55d97a2f221ede0f934dd9ad'
    assert len(b11['fallbacks']) == 1
    assert b11['fallbacks'][0]['type'] == 'P2PKH'
    assert b11['fallbacks'][0]['addr'] == '1RustyRX2oai4EYYDpQGWvEL62BBGqN9T'
    assert len(b11['routes']) == 1
    assert len(b11['routes'][0]) == 2
    assert b11['routes'][0][0]['pubkey'] == '029e03a901b85534ff1e92c43c74431f7ce72046060fcf7a95c37e148f78c77255'
    # 0x010203:0x040506:0x0708
    assert b11['routes'][0][0]['short_channel_id'] == '66051x263430x1800'
    assert b11['routes'][0][0]['fee_base_msat'] == 1
    assert b11['routes'][0][0]['fee_proportional_millionths'] == 20
    assert b11['routes'][0][0]['cltv_expiry_delta'] == 3

    assert b11['routes'][0][1]['pubkey'] == '039e03a901b85534ff1e92c43c74431f7ce72046060fcf7a95c37e148f78c77255'
    # 0x030405:0x060708:0x090a
    assert b11['routes'][0][1]['short_channel_id'] == '197637x395016x2314'
    assert b11['routes'][0][1]['fee_base_msat'] == 2
    assert b11['routes'][0][1]['fee_proportional_millionths'] == 30
    assert b11['routes'][0][1]['cltv_expiry_delta'] == 4

    # > ### On mainnet, with fallback (P2SH) address 3EktnHQD7RiAE6uzMj2ZifT9YgRrkSgzQX
    # > lnbc20m1pvjluezhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqspp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqfppj3a24vwu6r8ejrss3axul8rxldph2q7z9kmrgvr7xlaqm47apw3d48zm203kzcq357a4ls9al2ea73r8jcceyjtya6fu5wzzpe50zrge6ulk4nvjcpxlekvmxl6qcs9j3tz0469gq5g658y
    #
    # Breakdown:
    #
    # * `lnbc`: prefix, lightning on bitcoin mainnet
    # * `20m`: amount (20 milli-bitcoin)
    # * `1`: Bech32 separator
    # * `pvjluez`: timestamp (1496314658)
    # * `p`: payment hash...
    # * `f`: tagged field: fallback address.
    # * `pp`: `data_length` (`p` = 1. 1 * 32 + 1 == 33)
    # * `j3a24vwu6r8ejrss3axul8rxldph2q7z9`: `j` = 18, so P2SH address
    # * `h`: tagged field: hash of description...
    # * `2jhz8j78lv2jynuzmz6g8ve53he7pheeype33zlja5azae957585uu7x59w0f2l3rugyva6zpu394y4rh093j6wxze0ldsvk757a9msq`: signature
    # * `mf9swh`: Bech32 checksum
    b11 = l1.rpc.decodepay('lnbc20m1pvjluezhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqspp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqfppj3a24vwu6r8ejrss3axul8rxldph2q7z9kmrgvr7xlaqm47apw3d48zm203kzcq357a4ls9al2ea73r8jcceyjtya6fu5wzzpe50zrge6ulk4nvjcpxlekvmxl6qcs9j3tz0469gq5g658y', 'One piece of chocolate cake, one icecream cone, one pickle, one slice of swiss cheese, one slice of salami, one lollypop, one piece of cherry pie, one sausage, one cupcake, and one slice of watermelon')
    assert b11['currency'] == 'bc'
    assert b11['msatoshi'] == 20 * 10**11 // 1000
    assert b11['amount_msat'] == Millisatoshi(20 * 10**11 // 1000)
    assert b11['created_at'] == 1496314658
    assert b11['payment_hash'] == '0001020304050607080900010203040506070809000102030405060708090102'
    assert b11['expiry'] == 3600
    assert b11['payee'] == '03e7156ae33b0a208d0744199163177e909e80176e55d97a2f221ede0f934dd9ad'
    assert len(b11['fallbacks']) == 1
    assert b11['fallbacks'][0]['type'] == 'P2SH'
    assert b11['fallbacks'][0]['addr'] == '3EktnHQD7RiAE6uzMj2ZifT9YgRrkSgzQX'

    # > ### On mainnet, with fallback (P2WPKH) address bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4
    # > lnbc20m1pvjluezhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqspp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqfppqw508d6qejxtdg4y5r3zarvary0c5xw7kepvrhrm9s57hejg0p662ur5j5cr03890fa7k2pypgttmh4897d3raaq85a293e9jpuqwl0rnfuwzam7yr8e690nd2ypcq9hlkdwdvycqa0qza8
    #
    # * `lnbc`: prefix, lightning on bitcoin mainnet
    # * `20m`: amount (20 milli-bitcoin)
    # * `1`: Bech32 separator
    # * `pvjluez`: timestamp (1496314658)
    # * `p`: payment hash...
    # * `f`: tagged field: fallback address.
    # * `pp`: `data_length` (`p` = 1. 1 * 32 + 1 == 33)
    # * `q`: 0, so witness version 0.
    # * `qw508d6qejxtdg4y5r3zarvary0c5xw7k`: 160 bits = P2WPKH.
    # * `h`: tagged field: hash of description...
    # * `gw6tk8z0p0qdy9ulggx65lvfsg3nxxhqjxuf2fvmkhl9f4jc74gy44d5ua9us509prqz3e7vjxrftn3jnk7nrglvahxf7arye5llphgq`: signature
    # * `qdtpa4`: Bech32 checksum
    b11 = l1.rpc.decodepay('lnbc20m1pvjluezhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqspp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqfppqw508d6qejxtdg4y5r3zarvary0c5xw7kepvrhrm9s57hejg0p662ur5j5cr03890fa7k2pypgttmh4897d3raaq85a293e9jpuqwl0rnfuwzam7yr8e690nd2ypcq9hlkdwdvycqa0qza8', 'One piece of chocolate cake, one icecream cone, one pickle, one slice of swiss cheese, one slice of salami, one lollypop, one piece of cherry pie, one sausage, one cupcake, and one slice of watermelon')
    assert b11['currency'] == 'bc'
    assert b11['msatoshi'] == 20 * 10**11 // 1000
    assert b11['amount_msat'] == Millisatoshi(20 * 10**11 // 1000)
    assert b11['created_at'] == 1496314658
    assert b11['payment_hash'] == '0001020304050607080900010203040506070809000102030405060708090102'
    assert b11['expiry'] == 3600
    assert b11['payee'] == '03e7156ae33b0a208d0744199163177e909e80176e55d97a2f221ede0f934dd9ad'
    assert len(b11['fallbacks']) == 1
    assert b11['fallbacks'][0]['type'] == 'P2WPKH'
    assert b11['fallbacks'][0]['addr'] == 'bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4'

    # > ### On mainnet, with fallback (P2WSH) address bc1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3
    # > lnbc20m1pvjluezhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqspp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqfp4qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q28j0v3rwgy9pvjnd48ee2pl8xrpxysd5g44td63g6xcjcu003j3qe8878hluqlvl3km8rm92f5stamd3jw763n3hck0ct7p8wwj463cql26ava
    #
    # * `lnbc`: prefix, lightning on bitcoin mainnet
    # * `20m`: amount (20 milli-bitcoin)
    # * `1`: Bech32 separator
    # * `pvjluez`: timestamp (1496314658)
    # * `p`: payment hash...
    # * `f`: tagged field: fallback address.
    # * `p4`: `data_length` (`p` = 1, `4` = 21. 1 * 32 + 21 == 53)
    # * `q`: 0, so witness version 0.
    # * `rp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q`: 260 bits = P2WSH.
    # * `h`: tagged field: hash of description...
    # * `5yps56lmsvgcrf476flet6js02m93kgasews8q3jhtp7d6cqckmh70650maq4u65tk53ypszy77v9ng9h2z3q3eqhtc3ewgmmv2grasp`: signature
    # * `akvd7y`: Bech32 checksum
    b11 = l1.rpc.decodepay('lnbc20m1pvjluezhp58yjmdan79s6qqdhdzgynm4zwqd5d7xmw5fk98klysy043l2ahrqspp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqfp4qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q28j0v3rwgy9pvjnd48ee2pl8xrpxysd5g44td63g6xcjcu003j3qe8878hluqlvl3km8rm92f5stamd3jw763n3hck0ct7p8wwj463cql26ava', 'One piece of chocolate cake, one icecream cone, one pickle, one slice of swiss cheese, one slice of salami, one lollypop, one piece of cherry pie, one sausage, one cupcake, and one slice of watermelon')
    assert b11['currency'] == 'bc'
    assert b11['msatoshi'] == 20 * 10**11 // 1000
    assert b11['amount_msat'] == Millisatoshi(20 * 10**11 // 1000)
    assert b11['created_at'] == 1496314658
    assert b11['payment_hash'] == '0001020304050607080900010203040506070809000102030405060708090102'
    assert b11['expiry'] == 3600
    assert b11['payee'] == '03e7156ae33b0a208d0744199163177e909e80176e55d97a2f221ede0f934dd9ad'
    assert len(b11['fallbacks']) == 1
    assert b11['fallbacks'][0]['type'] == 'P2WSH'
    assert b11['fallbacks'][0]['addr'] == 'bc1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3'

    with pytest.raises(RpcError):
        l1.rpc.decodepay('1111111')


@unittest.skipIf(not DEVELOPER, "Too slow without --dev-bitcoind-poll")
def test_forward(node_factory, bitcoind):
    # Connect 1 -> 2 -> 3.
    l1, l2, l3 = node_factory.line_graph(3, fundchannel=True)

    # Allow announce messages.
    l1.bitcoin.generate_block(5)

    # If they're at different block heights we can get spurious errors.
    sync_blockheight(bitcoind, [l1, l2, l3])

    chanid1 = only_one(l1.rpc.getpeer(l2.info['id'])['channels'])['short_channel_id']
    chanid2 = only_one(l2.rpc.getpeer(l3.info['id'])['channels'])['short_channel_id']
    assert only_one(l2.rpc.getpeer(l1.info['id'])['channels'])['short_channel_id'] == chanid1
    assert only_one(l3.rpc.getpeer(l2.info['id'])['channels'])['short_channel_id'] == chanid2

    rhash = l3.rpc.invoice(100000000, 'testpayment1', 'desc')['payment_hash']
    assert only_one(l3.rpc.listinvoices('testpayment1')['invoices'])['status'] == 'unpaid'

    # Fee for node2 is 10 millionths, plus 1.
    amt = 100000000
    fee = amt * 10 // 1000000 + 1

    baseroute = [{'msatoshi': amt + fee,
                  'id': l2.info['id'],
                  'delay': 12,
                  'channel': chanid1},
                 {'msatoshi': amt,
                  'id': l3.info['id'],
                  'delay': 6,
                  'channel': chanid2}]

    # Unknown other peer
    route = copy.deepcopy(baseroute)
    route[1]['id'] = '031a8dc444e41bb989653a4501e11175a488a57439b0c4947704fd6e3de5dca607'
    l1.rpc.sendpay(route, rhash)
    with pytest.raises(RpcError):
        l1.rpc.waitsendpay(rhash)

    # Delay too short (we always add one internally anyway, so subtract 2 here).
    route = copy.deepcopy(baseroute)
    route[0]['delay'] = 8
    l1.rpc.sendpay(route, rhash)
    with pytest.raises(RpcError):
        l1.rpc.waitsendpay(rhash)

    # Final delay too short
    route = copy.deepcopy(baseroute)
    route[1]['delay'] = 3
    l1.rpc.sendpay(route, rhash)
    with pytest.raises(RpcError):
        l1.rpc.waitsendpay(rhash)

    # This one works
    route = copy.deepcopy(baseroute)
    l1.rpc.sendpay(route, rhash)
    l1.rpc.waitsendpay(rhash)


@unittest.skipIf(not DEVELOPER, "needs DEVELOPER=1 for --dev-broadcast-interval")
def test_forward_different_fees_and_cltv(node_factory, bitcoind):
    # FIXME: Check BOLT quotes here too
    # BOLT #7:
    # ```
    #    B
    #   / \
    #  /   \
    # A     C
    #  \   /
    #   \ /
    #    D
    # ```
    #
    # Each advertises the following `cltv_expiry_delta` on its end of every
    # channel:
    #
    # 1. A: 10 blocks
    # 2. B: 20 blocks
    # 3. C: 30 blocks
    # 4. D: 40 blocks
    #
    # C also uses a minimum `cltv_expiry` of 9 (the default) when requesting
    # payments.
    #
    # Also, each node has the same fee scheme which it uses for each of its
    # channels:
    #
    # 1. A: 100 base + 1000 millionths
    # 1. B: 200 base + 2000 millionths
    # 1. C: 300 base + 3000 millionths
    # 1. D: 400 base + 4000 millionths

    # We don't do D yet.
    l1 = node_factory.get_node(options={'cltv-delta': 10, 'fee-base': 100, 'fee-per-satoshi': 1000})
    l2 = node_factory.get_node(options={'cltv-delta': 20, 'fee-base': 200, 'fee-per-satoshi': 2000})
    l3 = node_factory.get_node(options={'cltv-delta': 30, 'cltv-final': 9, 'fee-base': 300, 'fee-per-satoshi': 3000})

    ret = l1.rpc.connect(l2.info['id'], 'localhost', l2.port)
    assert ret['id'] == l2.info['id']

    l1.daemon.wait_for_log('openingd-.*: Handed peer, entering loop')
    l2.daemon.wait_for_log('openingd-.*: Handed peer, entering loop')

    ret = l2.rpc.connect(l3.info['id'], 'localhost', l3.port)
    assert ret['id'] == l3.info['id']

    l2.daemon.wait_for_log('openingd-.*: Handed peer, entering loop')
    l3.daemon.wait_for_log('openingd-.*: Handed peer, entering loop')

    c1 = l1.fund_channel(l2, 10**6)
    c2 = l2.fund_channel(l3, 10**6)
    bitcoind.generate_block(5)

    # Make sure l1 has seen announce for all channels.
    l1.wait_channel_active(c1)
    l1.wait_channel_active(c2)

    # BOLT #7:
    #
    # If B were to send 4,999,999 millisatoshi directly to C, it wouldn't
    # charge itself a fee nor add its own `cltv_expiry_delta`, so it would
    # use C's requested `cltv_expiry` of 9.  We also assume it adds a
    # "shadow route" to give an extra CLTV of 42.  It could also add extra
    # cltv deltas at other hops, as these values are a minimum, but we don't
    # here for simplicity:

    # FIXME: Add shadow route
    shadow_route = 0
    route = l2.rpc.getroute(l3.info['id'], 4999999, 1)["route"]
    assert len(route) == 1

    # BOLT #7:
    #
    #    * `amount_msat`: 4999999
    #    * `cltv_expiry`: current-block-height + 9 + 42
    #    * `onion_routing_packet`:
    #      * `amt_to_forward` = 4999999
    #      * `outgoing_cltv_value` = current-block-height + 9 + 42
    #
    assert route[0]['msatoshi'] == 4999999
    assert route[0]['delay'] == 9 + shadow_route

    # BOLT #7:
    # If A were to send 4,999,999 millisatoshi to C via B, it needs to
    # pay B the fee it specified in the B->C `channel_update`, calculated as
    # per [HTLC Fees](#htlc_fees):
    #
    # 200 + 4999999 * 2000 / 1000000 = 10199
    #
    # Similarly, it would need to add the `cltv_expiry` from B->C's
    # `channel_update` (20), plus C's requested minimum (9), plus 42 for the
    # "shadow route".  Thus the `update_add_htlc` message from A to B would
    # be:
    #
    #    * `amount_msat`: 5010198
    #    * `cltv_expiry`: current-block-height + 20 + 9 + 42
    #    * `onion_routing_packet`:
    #      * `amt_to_forward` = 4999999
    #      * `outgoing_cltv_value` = current-block-height + 9 + 42
    route = l1.rpc.getroute(l3.info['id'], 4999999, 1)["route"]
    assert len(route) == 2

    assert route[0]['msatoshi'] == 5010198
    assert route[0]['delay'] == 20 + 9 + shadow_route
    assert route[1]['msatoshi'] == 4999999
    assert route[1]['delay'] == 9 + shadow_route

    rhash = l3.rpc.invoice(4999999, 'test_forward_different_fees_and_cltv', 'desc')['payment_hash']
    assert only_one(l3.rpc.listinvoices('test_forward_different_fees_and_cltv')['invoices'])['status'] == 'unpaid'

    # This should work.
    l1.rpc.sendpay(route, rhash)
    l1.rpc.waitsendpay(rhash)

    # We add one to the blockcount for a bit of fuzz (FIXME: Shadowroute would fix this!)
    shadow_route = 1
    l1.daemon.wait_for_log("Adding HTLC 0 amount=5010198msat cltv={} gave CHANNEL_ERR_ADD_OK"
                           .format(bitcoind.rpc.getblockcount() + 20 + 9 + shadow_route))
    l2.daemon.wait_for_log("Adding HTLC 0 amount=4999999msat cltv={} gave CHANNEL_ERR_ADD_OK"
                           .format(bitcoind.rpc.getblockcount() + 9 + shadow_route))
    l3.daemon.wait_for_log("Resolved invoice 'test_forward_different_fees_and_cltv' with amount 4999999msat")
    assert only_one(l3.rpc.listinvoices('test_forward_different_fees_and_cltv')['invoices'])['status'] == 'paid'

    # Check that we see all the channels
    shortids = set(c['short_channel_id'] for c in l2.rpc.listchannels()['channels'])
    for scid in shortids:
        c = l1.rpc.listchannels(scid)['channels']
        # We get one entry for each direction.
        assert len(c) == 2
        assert c[0]['short_channel_id'] == scid
        assert c[1]['short_channel_id'] == scid
        assert c[0]['source'] == c[1]['destination']
        assert c[1]['source'] == c[0]['destination']


@unittest.skipIf(not DEVELOPER, "needs DEVELOPER=1 for --dev-broadcast-interval")
def test_forward_pad_fees_and_cltv(node_factory, bitcoind):
    """Test that we are allowed extra locktime delta, and fees"""

    l1 = node_factory.get_node(options={'cltv-delta': 10, 'fee-base': 100, 'fee-per-satoshi': 1000})
    l2 = node_factory.get_node(options={'cltv-delta': 20, 'fee-base': 200, 'fee-per-satoshi': 2000})
    l3 = node_factory.get_node(options={'cltv-delta': 30, 'cltv-final': 9, 'fee-base': 300, 'fee-per-satoshi': 3000})

    ret = l1.rpc.connect(l2.info['id'], 'localhost', l2.port)
    assert ret['id'] == l2.info['id']

    l1.daemon.wait_for_log('openingd-.*: Handed peer, entering loop')
    l2.daemon.wait_for_log('openingd-.*: Handed peer, entering loop')

    ret = l2.rpc.connect(l3.info['id'], 'localhost', l3.port)
    assert ret['id'] == l3.info['id']

    l2.daemon.wait_for_log('openingd-.*: Handed peer, entering loop')
    l3.daemon.wait_for_log('openingd-.*: Handed peer, entering loop')

    c1 = l1.fund_channel(l2, 10**6)
    c2 = l2.fund_channel(l3, 10**6)
    bitcoind.generate_block(5)

    # Make sure l1 has seen announce for all channels.
    l1.wait_channel_active(c1)
    l1.wait_channel_active(c2)

    route = l1.rpc.getroute(l3.info['id'], 4999999, 1)["route"]
    assert len(route) == 2

    assert route[0]['msatoshi'] == 5010198
    assert route[0]['delay'] == 20 + 9
    assert route[1]['msatoshi'] == 4999999
    assert route[1]['delay'] == 9

    # Modify so we overpay, overdo the cltv.
    route[0]['msatoshi'] += 2000
    route[0]['amount_msat'] = Millisatoshi(route[0]['msatoshi'])
    route[0]['delay'] += 20
    route[1]['msatoshi'] += 1000
    route[1]['amount_msat'] = Millisatoshi(route[1]['msatoshi'])
    route[1]['delay'] += 10

    # This should work.
    rhash = l3.rpc.invoice(4999999, 'test_forward_pad_fees_and_cltv', 'desc')['payment_hash']
    l1.rpc.sendpay(route, rhash)
    l1.rpc.waitsendpay(rhash)
    assert only_one(l3.rpc.listinvoices('test_forward_pad_fees_and_cltv')['invoices'])['status'] == 'paid'


@unittest.skipIf(not DEVELOPER, "needs DEVELOPER=1 for dev_ignore_htlcs")
def test_forward_stats(node_factory, bitcoind):
    """Check that we track forwarded payments correctly.

    We wire up the network to have l1 as payment initiator, l2 as
    forwarded (the one we check) and l3-l5 as payment recipients. l3
    accepts correctly, l4 rejects (because it doesn't know the payment
    hash) and l5 will keep the HTLC dangling by disconnecting.

    """
    amount = 10**5
    l1, l2, l3 = node_factory.line_graph(3, wait_for_announce=False)
    l4 = node_factory.get_node()
    l5 = node_factory.get_node(may_fail=True)
    l2.openchannel(l4, 10**6, wait_for_announce=False)
    l2.openchannel(l5, 10**6, wait_for_announce=True)

    bitcoind.generate_block(5)

    wait_for(lambda: len(l1.rpc.listchannels()['channels']) == 8)

    payment_hash = l3.rpc.invoice(amount, "first", "desc")['payment_hash']
    route = l1.rpc.getroute(l3.info['id'], amount, 1)['route']

    l1.rpc.sendpay(route, payment_hash)
    l1.rpc.waitsendpay(payment_hash)

    # l4 rejects since it doesn't know the payment_hash
    route = l1.rpc.getroute(l4.info['id'], amount, 1)['route']
    payment_hash = "F" * 64
    with pytest.raises(RpcError):
        l1.rpc.sendpay(route, payment_hash)
        l1.rpc.waitsendpay(payment_hash)

    # l5 will hold the HTLC hostage.
    l5.rpc.dev_ignore_htlcs(id=l2.info['id'], ignore=True)
    route = l1.rpc.getroute(l5.info['id'], amount, 1)['route']
    payment_hash = l5.rpc.invoice(amount, "first", "desc")['payment_hash']
    l1.rpc.sendpay(route, payment_hash)

    l5.daemon.wait_for_log(r'their htlc .* dev_ignore_htlcs')

    # Select all forwardings, ordered by htlc_id to ensure the order
    # matches below
    forwardings = l2.db_query("SELECT *, in_msatoshi - out_msatoshi as fee "
                              "FROM forwarded_payments "
                              "ORDER BY in_htlc_id;")
    assert(len(forwardings) == 3)
    states = [f['state'] for f in forwardings]
    assert(states == [1, 2, 0])  # settled, failed, offered

    inchan = l2.rpc.listpeers(l1.info['id'])['peers'][0]['channels'][0]
    outchan = l2.rpc.listpeers(l3.info['id'])['peers'][0]['channels'][0]

    # Check that we correctly account channel changes
    assert inchan['in_payments_offered'] == 3
    assert inchan['in_payments_fulfilled'] == 1
    assert inchan['in_msatoshi_offered'] >= 3 * amount
    assert inchan['in_msatoshi_fulfilled'] >= amount

    assert outchan['out_payments_offered'] == 1
    assert outchan['out_payments_fulfilled'] == 1
    assert outchan['out_msatoshi_offered'] >= amount
    assert outchan['out_msatoshi_offered'] == outchan['out_msatoshi_fulfilled']

    assert outchan['out_msatoshi_fulfilled'] < inchan['in_msatoshi_fulfilled']

    stats = l2.rpc.listforwards()

    assert [f['status'] for f in stats['forwards']] == ['settled', 'failed', 'offered']
    assert l2.rpc.getinfo()['msatoshi_fees_collected'] == 1 + amount // 100000
    assert l1.rpc.getinfo()['msatoshi_fees_collected'] == 0
    assert l3.rpc.getinfo()['msatoshi_fees_collected'] == 0
    assert stats['forwards'][0]['received_time'] <= stats['forwards'][0]['resolved_time']
    assert stats['forwards'][1]['received_time'] <= stats['forwards'][1]['resolved_time']
    assert 'received_time' in stats['forwards'][2] and 'resolved_time' not in stats['forwards'][2]


@unittest.skipIf(not DEVELOPER, "needs DEVELOPER=1")
def test_forward_local_failed_stats(node_factory, bitcoind, executor):
    """Check that we track forwarded payments correctly.

    We wire up the network to have l1 and l6 as payment initiator, l2 as
    forwarded (the one we check) and l3-l5 as payment recipients.

    There 5 cases for FORWARD_LOCAL_FAILED status:
    1. When Msater resolves the reply about the next peer infor(sent
       by Gossipd), and need handle unknown next peer failure in
       channel_resolve_reply(). For this case, we ask l1 pay to l3
       through l2 but close the channel between l2 and l3 after
       getroute(), the payment will fail in l2 because of
       WIRE_UNKNOWN_NEXT_PEER;
    2. When Master handle the forward process with the htlc_in and
       the id of next hop, it tries to drive a new htlc_out but fails
       in forward_htlc(). For this case, we ask l1 pay to 14 through
       with no fee, so the payment will fail in l2 becase of
       WIRE_FEE_INSUFFICIENT;
    3. When we send htlc_out, Master asks Channeld to add a new htlc
       into the outgoing channel but Channeld fails. Master need
       handle and store this failure in rcvd_htlc_reply(). For this
       case, we ask l1 pay to l5 with 10**8 sat though the channel
       (l2-->l5) with the max capacity of 10**4 msat , the payment
       will fail in l2 because of CHANNEL_ERR_MAX_HTLC_VALUE_EXCEEDED;
    4. When Channeld receives a new revoke message, if the state of
       corresponding htlc is RCVD_ADD_ACK_REVOCATION, Master will tries
       to resolve onionpacket and handle the failure before resolving
       the next hop in peer_got_revoke(). For this case, we ask l6 pay
       to l4 though l1 and l2, but we replace the second node_id in route
       with the wrong one, so the payment will fail in l2 because of
       WIRE_INVALID_ONION_KEY;
    5. When Onchaind finds the htlc time out or missing htlc, Master
       need handle these failure as FORWARD_LOCAL_FAILED in if it's forward
       payment case. For this case, we ask l1 pay to l4 though l2 with the
       amount less than the invoice(the payment must fail in l4), and we
       also ask l5 disconnected before sending update_fail_htlc, so the
       htlc will be holding until l2 meets timeout and handle it as local_fail.
    """

    amount = 10**8

    disconnects = ['-WIRE_UPDATE_FAIL_HTLC', 'permfail']

    l1 = node_factory.get_node()
    l2 = node_factory.get_node()
    l3 = node_factory.get_node()
    l4 = node_factory.get_node(disconnect=disconnects)
    l5 = node_factory.get_node()
    l6 = node_factory.get_node()

    l1.rpc.connect(l2.info['id'], 'localhost', l2.port)
    l2.rpc.connect(l3.info['id'], 'localhost', l3.port)
    l2.rpc.connect(l4.info['id'], 'localhost', l4.port)
    l2.rpc.connect(l5.info['id'], 'localhost', l5.port)
    l6.rpc.connect(l1.info['id'], 'localhost', l1.port)
    c12 = l1.fund_channel(l2, 10**6)
    c23 = l2.fund_channel(l3, 10**6)
    c24 = l2.fund_channel(l4, 10**6)
    c25 = l2.fund_channel(l5, 10**4)
    l6.fund_channel(l1, 10**6)

    # Make sure routes finalized.
    bitcoind.generate_block(5)
    l1.wait_channel_active(c23)
    l1.wait_channel_active(c24)
    l1.wait_channel_active(c25)

    wait_for(lambda: len(l1.rpc.listchannels()['channels']) == 10)

    """1. When Msater resolves the reply about the next peer infor(sent
       by Gossipd), and need handle unknown next peer failure in
       channel_resolve_reply();

       For this case, we ask l1 pay to l3 through l2 but close the channel
       between l2 and l3 after getroute(), the payment will fail in l2
       because of WIRE_UNKNOWN_NEXT_PEER;
    """

    payment_hash = l3.rpc.invoice(amount, "first", "desc")['payment_hash']
    route = l1.rpc.getroute(l3.info['id'], amount, 1)['route']

    l2.rpc.close(c23, True, 0)

    with pytest.raises(RpcError):
        l1.rpc.sendpay(route, payment_hash)
        l1.rpc.waitsendpay(payment_hash)

    """2. When Master handle the forward process with the htlc_in and
       the id of next hop, it tries to drive a new htlc_out but fails
       in forward_htlc();

       For this case, we ask l1 pay to 14 through with no fee, so the
       payment will fail in l2 becase of WIRE_FEE_INSUFFICIENT;
    """

    payment_hash = l4.rpc.invoice(amount, "third", "desc")['payment_hash']
    fee = amount * 10 // 1000000 + 1

    route = [{'msatoshi': amount,
              'id': l2.info['id'],
              'delay': 12,
              'channel': c12},
             {'msatoshi': amount,
              'id': l4.info['id'],
              'delay': 6,
              'channel': c24}]

    with pytest.raises(RpcError):
        l1.rpc.sendpay(route, payment_hash)
        l1.rpc.waitsendpay(payment_hash)

    """3. When we send htlc_out, Master asks Channeld to add a new htlc
       into the outgoing channel but Channeld fails. Master need
       handle and store this failure in rcvd_htlc_reply();

       For this case, we ask l1 pay to l5 with 10**8 sat though the channel
       (l2-->l5) with the max capacity of 10**4 msat , the payment will
       fail in l2 because of CHANNEL_ERR_MAX_HTLC_VALUE_EXCEEDED;
    """

    payment_hash = l5.rpc.invoice(amount, "second", "desc")['payment_hash']
    fee = amount * 10 // 1000000 + 1

    route = [{'msatoshi': amount + fee,
              'id': l2.info['id'],
              'delay': 12,
              'channel': c12},
             {'msatoshi': amount,
              'id': l5.info['id'],
              'delay': 6,
              'channel': c25}]

    with pytest.raises(RpcError):
        l1.rpc.sendpay(route, payment_hash)
        l1.rpc.waitsendpay(payment_hash)

    """4. When Channeld receives a new revoke message, if the state of
       corresponding htlc is RCVD_ADD_ACK_REVOCATION, Master will tries
       to resolve onionpacket and handle the failure before resolving
       the next hop in peer_got_revoke();

       For this case, we ask l6 pay to l4 though l1 and l2, but we replace
       the second node_id in route with the wrong one, so the payment will
       fail in l2 because of WIRE_INVALID_ONION_KEY;
    """

    payment_hash = l4.rpc.invoice(amount, 'fourth', 'desc')['payment_hash']
    route = l6.rpc.getroute(l4.info['id'], amount, 1)['route']

    mangled_nodeid = '0265b6ab5ec860cd257865d61ef0bbf5b3339c36cbda8b26b74e7f1dca490b6510'

    # Replace id with a different pubkey, so onion encoded badly at l2 hop.
    route[1]['id'] = mangled_nodeid

    with pytest.raises(RpcError):
        l6.rpc.sendpay(route, payment_hash)
        l6.rpc.waitsendpay(payment_hash)

    """5. When Onchaind finds the htlc time out or missing htlc, Master
       need handle these failure as FORWARD_LOCAL_FAILED in if it's forward
       payment case.

       For this case, we ask l1 pay to l4 though l2 with the amount less than
       the invoice(the payment must fail in l4), and we also ask l5 disconnected
       before sending update_fail_htlc, so the htlc will be holding until l2
       meets timeout and handle it as local_fail.
    """
    payment_hash = l4.rpc.invoice(amount, 'onchain_timeout', 'desc')['payment_hash']
    fee = amount * 10 // 1000000 + 1

    # We underpay, so it fails.
    route = [{'msatoshi': amount + fee - 1,
              'id': l2.info['id'],
              'delay': 12,
              'channel': c12},
             {'msatoshi': amount - 1,
              'id': l4.info['id'],
              'delay': 5,
              'channel': c24}]

    executor.submit(l1.rpc.sendpay, route, payment_hash)

    l4.daemon.wait_for_log('permfail')
    l4.wait_for_channel_onchain(l2.info['id'])
    l2.bitcoin.generate_block(1)
    l2.daemon.wait_for_log(' to ONCHAIN')
    l4.daemon.wait_for_log(' to ONCHAIN')

    # Wait for timeout.
    l2.daemon.wait_for_log('Propose handling THEIR_UNILATERAL/OUR_HTLC by OUR_HTLC_TIMEOUT_TO_US .* after 6 blocks')
    bitcoind.generate_block(6)

    l2.wait_for_onchaind_broadcast('OUR_HTLC_TIMEOUT_TO_US',
                                   'THEIR_UNILATERAL/OUR_HTLC')

    bitcoind.generate_block(1)
    l2.daemon.wait_for_log('Resolved THEIR_UNILATERAL/OUR_HTLC by our proposal OUR_HTLC_TIMEOUT_TO_US')
    l4.daemon.wait_for_log('Ignoring output.*: OUR_UNILATERAL/THEIR_HTLC')

    bitcoind.generate_block(100)
    sync_blockheight(bitcoind, [l2])

    # give time to let l2 store the local_failed stats
    time.sleep(5)

    # Select all forwardings, and check the status
    stats = l2.rpc.listforwards()

    assert [f['status'] for f in stats['forwards']] == ['local_failed', 'local_failed', 'local_failed', 'local_failed', 'local_failed']
    assert l2.rpc.getinfo()['msatoshi_fees_collected'] == 0

    assert 'received_time' in stats['forwards'][0] and 'resolved_time' not in stats['forwards'][0]
    assert 'received_time' in stats['forwards'][1] and 'resolved_time' not in stats['forwards'][1]
    assert 'received_time' in stats['forwards'][2] and 'resolved_time' not in stats['forwards'][2]
    assert 'received_time' in stats['forwards'][3] and 'resolved_time' not in stats['forwards'][3]
    assert 'received_time' in stats['forwards'][3] and 'resolved_time' not in stats['forwards'][4]


@unittest.skipIf(not DEVELOPER or SLOW_MACHINE, "needs DEVELOPER=1 for dev_ignore_htlcs, and temporarily disabled on Travis")
def test_htlcs_cltv_only_difference(node_factory, bitcoind):
    # l1 -> l2 -> l3 -> l4
    # l4 ignores htlcs, so they stay.
    # l3 will see a reconnect from l4 when l4 restarts.
    l1, l2, l3, l4 = node_factory.line_graph(4, wait_for_announce=True, opts=[{}] * 2 + [{'dev-no-reconnect': None, 'may_reconnect': True}] * 2)

    h = l4.rpc.invoice(msatoshi=10**8, label='x', description='desc')['payment_hash']
    l4.rpc.dev_ignore_htlcs(id=l3.info['id'], ignore=True)

    # L2 tries to pay
    r = l2.rpc.getroute(l4.info['id'], 10**8, 1)["route"]
    l2.rpc.sendpay(r, h)

    # Now increment CLTV
    bitcoind.generate_block(1)
    sync_blockheight(bitcoind, [l1, l2, l3, l4])

    # L1 tries to pay
    r = l1.rpc.getroute(l4.info['id'], 10**8, 1)["route"]
    l1.rpc.sendpay(r, h)

    # Now increment CLTV
    bitcoind.generate_block(1)
    sync_blockheight(bitcoind, [l1, l2, l3, l4])

    # L3 tries to pay
    r = l3.rpc.getroute(l4.info['id'], 10**8, 1)["route"]
    l3.rpc.sendpay(r, h)

    # Give them time to go through.
    time.sleep(5)

    # Will all be connected OK.
    assert only_one(l1.rpc.listpeers(l2.info['id'])['peers'])['connected']
    assert only_one(l2.rpc.listpeers(l3.info['id'])['peers'])['connected']
    assert only_one(l3.rpc.listpeers(l4.info['id'])['peers'])['connected']

    # TODO Remove our reliance on HTLCs failing on startup and the need for
    #      this plugin
    l4.daemon.opts['plugin'] = 'tests/plugins/fail_htlcs.py'

    # Restarting tail node will stop it ignoring HTLCs (it will actually
    # fail them immediately).
    l4.restart()
    l3.rpc.connect(l4.info['id'], 'localhost', l4.port)

    wait_for(lambda: only_one(l1.rpc.listsendpays()['payments'])['status'] == 'failed')
    wait_for(lambda: only_one(l2.rpc.listsendpays()['payments'])['status'] == 'failed')
    wait_for(lambda: only_one(l3.rpc.listsendpays()['payments'])['status'] == 'failed')

    # Should all still be connected.
    assert only_one(l1.rpc.listpeers(l2.info['id'])['peers'])['connected']
    assert only_one(l2.rpc.listpeers(l3.info['id'])['peers'])['connected']
    assert only_one(l3.rpc.listpeers(l4.info['id'])['peers'])['connected']


def test_pay_variants(node_factory):
    l1, l2 = node_factory.line_graph(2)

    # Upper case is allowed
    b11 = l2.rpc.invoice(123000, 'test_pay_variants upper', 'description')['bolt11'].upper()
    l1.rpc.decodepay(b11)
    l1.rpc.pay(b11)

    # lightning: prefix is allowed
    b11 = 'lightning:' + l2.rpc.invoice(123000, 'test_pay_variants with prefix', 'description')['bolt11']
    l1.rpc.decodepay(b11)
    l1.rpc.pay(b11)

    # BOTH is allowed.
    b11 = 'LIGHTNING:' + l2.rpc.invoice(123000, 'test_pay_variants upper with prefix', 'description')['bolt11'].upper()
    l1.rpc.decodepay(b11)
    l1.rpc.pay(b11)


@unittest.skipIf(not DEVELOPER, "gossip without DEVELOPER=1 is slow")
def test_pay_retry(node_factory, bitcoind):
    """Make sure pay command retries properly. """
    def exhaust_channel(funder, fundee, scid, already_spent=0):
        """Spend all available capacity (10^6 - 1%) of channel"""
        maxpay = (10**6 - 10**6 // 100 - 13440) * 1000 - already_spent
        inv = fundee.rpc.invoice(maxpay,
                                 ''.join(random.choice(string.ascii_letters + string.digits) for _ in range(20)),
                                 "exhaust_channel")
        routestep = {
            'msatoshi': maxpay,
            'id': fundee.info['id'],
            'delay': 10,
            'channel': scid
        }
        funder.rpc.sendpay([routestep], inv['payment_hash'])
        funder.rpc.waitsendpay(inv['payment_hash'])

    # We connect every node to l5; in a line and individually.
    # Keep fixed fees so we can easily calculate exhaustion
    l1, l2, l3, l4, l5 = node_factory.line_graph(5, fundchannel=False,
                                                 opts={'feerates': (7500, 7500, 7500)})

    # scid12
    l1.fund_channel(l2, 10**6, wait_for_active=False)
    # scid23
    l2.fund_channel(l3, 10**6, wait_for_active=False)
    # scid34
    l3.fund_channel(l4, 10**6, wait_for_active=False)
    scid45 = l4.fund_channel(l5, 10**6, wait_for_active=False)

    l1.rpc.connect(l5.info['id'], 'localhost', l5.port)
    scid15 = l1.fund_channel(l5, 10**6, wait_for_active=False)
    l2.rpc.connect(l5.info['id'], 'localhost', l5.port)
    scid25 = l2.fund_channel(l5, 10**6, wait_for_active=False)
    l3.rpc.connect(l5.info['id'], 'localhost', l5.port)
    scid35 = l3.fund_channel(l5, 10**6, wait_for_active=False)

    # Make sure l1 sees all 7 channels
    bitcoind.generate_block(5)
    wait_for(lambda: len(l1.rpc.listchannels()['channels']) == 14)

    # Exhaust shortcut channels one at a time, to force retries.
    exhaust_channel(l1, l5, scid15)
    exhaust_channel(l2, l5, scid25)
    exhaust_channel(l3, l5, scid35)

    def listpays_nofail(b11):
        while True:
            pays = l1.rpc.listpays(b11)['pays']
            if len(pays) != 0:
                if only_one(pays)['status'] == 'complete':
                    return
                assert only_one(pays)['status'] != 'failed'

    inv = l5.rpc.invoice(10**8, 'test_retry', 'test_retry')

    # Make sure listpays doesn't transiently show failure while pay
    # is retrying.
    executor = concurrent.futures.ThreadPoolExecutor()
    fut = executor.submit(listpays_nofail, inv['bolt11'])

    # Pay l1->l5 should succeed via straight line (eventually)
    l1.rpc.pay(inv['bolt11'])

    # This should be OK.
    fut.result()

    # This should make it fail.
    exhaust_channel(l4, l5, scid45, 10**8)

    with pytest.raises(RpcError, match=r'5 attempts'):
        l1.rpc.pay(l5.rpc.invoice(10**8, 'test_retry2', 'test_retry2')['bolt11'])


@unittest.skipIf(not DEVELOPER, "needs DEVELOPER=1 otherwise gossip takes 5 minutes!")
def test_pay_routeboost(node_factory, bitcoind):
    """Make sure we can use routeboost information. """
    # l1->l2->l3--private-->l4
    l1, l2 = node_factory.line_graph(2, announce_channels=True, wait_for_announce=True)
    l3, l4, l5 = node_factory.line_graph(3, announce_channels=False, wait_for_announce=False)

    # This should a "could not find a route" because that's true.
    with pytest.raises(RpcError, match=r'Could not find a route'):
        l1.rpc.pay(l5.rpc.invoice(10**8, 'test_retry', 'test_retry')['bolt11'])

    l2.rpc.connect(l3.info['id'], 'localhost', l3.port)
    scidl2l3 = l2.fund_channel(l3, 10**6)

    # Make sure l1 knows about the 2->3 channel.
    bitcoind.generate_block(5)
    l1.daemon.wait_for_logs([r'update for channel {}/0 now ACTIVE'
                             .format(scidl2l3),
                             r'update for channel {}/1 now ACTIVE'
                             .format(scidl2l3)])
    # Make sure l4 knows about 2->3 channel too so it's not a dead-end.
    l4.daemon.wait_for_logs([r'update for channel {}/0 now ACTIVE'
                             .format(scidl2l3),
                             r'update for channel {}/1 now ACTIVE'
                             .format(scidl2l3)])

    # Get an l4 invoice; it should put the private channel in routeboost.
    inv = l4.rpc.invoice(10**5, 'test_pay_routeboost', 'test_pay_routeboost',
                         exposeprivatechannels=True)
    assert 'warning_capacity' not in inv
    assert 'warning_offline' not in inv
    assert only_one(only_one(l1.rpc.decodepay(inv['bolt11'])['routes']))

    # Now we should be able to pay it.
    start = time.time()
    l1.rpc.pay(inv['bolt11'])
    end = time.time()

    # Status should show all the gory details.
    status = l1.rpc.call('paystatus', [inv['bolt11']])
    assert only_one(status['pay'])['bolt11'] == inv['bolt11']
    assert only_one(status['pay'])['msatoshi'] == 10**5
    assert only_one(status['pay'])['amount_msat'] == Millisatoshi(10**5)
    assert only_one(status['pay'])['destination'] == l4.info['id']
    assert 'label' not in only_one(status['pay'])
    assert 'routehint_modifications' not in only_one(status['pay'])
    assert 'local_exclusions' not in only_one(status['pay'])
    # First attempt will fail, then it will try route hint
    attempts = only_one(status['pay'])['attempts']
    assert len(attempts) == 2
    assert attempts[0]['strategy'] == "Initial attempt"
    # FIXME!
    PAY_ROUTE_NOT_FOUND = 205
    assert attempts[0]['failure']['code'] == PAY_ROUTE_NOT_FOUND
    assert attempts[1]['strategy'] == "Trying route hint"
    assert 'success' in attempts[1]
    assert attempts[1]['age_in_seconds'] <= time.time() - start
    assert attempts[1]['duration_in_seconds'] <= end - start
    assert only_one(attempts[1]['routehint'])
    assert only_one(attempts[1]['routehint'])['id'] == l3.info['id']
    scid34 = only_one(l3.rpc.listpeers(l4.info['id'])['peers'])['channels'][0]['short_channel_id']
    assert only_one(attempts[1]['routehint'])['channel'] == scid34
    assert only_one(attempts[1]['routehint'])['fee_base_msat'] == 1
    assert only_one(attempts[1]['routehint'])['fee_proportional_millionths'] == 10
    assert only_one(attempts[1]['routehint'])['cltv_expiry_delta'] == 6

    # With dev-route option we can test longer routehints.
    if DEVELOPER:
        scid45 = only_one(l4.rpc.listpeers(l5.info['id'])['peers'])['channels'][0]['short_channel_id']
        routel3l4l5 = [{'id': l3.info['id'],
                        'short_channel_id': scid34,
                        'fee_base_msat': 1000,
                        'fee_proportional_millionths': 10,
                        'cltv_expiry_delta': 6},
                       {'id': l4.info['id'],
                        'short_channel_id': scid45,
                        'fee_base_msat': 1000,
                        'fee_proportional_millionths': 10,
                        'cltv_expiry_delta': 6}]
        inv = l5.rpc.call('invoice', {'msatoshi': 10**5,
                                      'label': 'test_pay_routeboost2',
                                      'description': 'test_pay_routeboost2',
                                      'dev-routes': [routel3l4l5]})
        l1.rpc.pay(inv['bolt11'])
        status = l1.rpc.call('paystatus', [inv['bolt11']])
        assert len(only_one(status['pay'])['attempts']) == 2
        assert 'failure' in only_one(status['pay'])['attempts'][0]
        assert 'success' not in only_one(status['pay'])['attempts'][0]
        assert 'failure' not in only_one(status['pay'])['attempts'][1]
        assert 'success' in only_one(status['pay'])['attempts'][1]

        # Finally, it should fall back to second routehint if first fails.
        # (Note, this is not public because it's not 6 deep)
        l3.rpc.connect(l5.info['id'], 'localhost', l5.port)
        scid35 = l3.fund_channel(l5, 10**6)
        l4.stop()
        routel3l5 = [{'id': l3.info['id'],
                      'short_channel_id': scid35,
                      'fee_base_msat': 1000,
                      'fee_proportional_millionths': 10,
                      'cltv_expiry_delta': 6}]
        inv = l5.rpc.call('invoice', {'msatoshi': 10**5,
                                      'label': 'test_pay_routeboost5',
                                      'description': 'test_pay_routeboost5',
                                      'dev-routes': [routel3l4l5, routel3l5]})
        l1.rpc.pay(inv['bolt11'], label="paying test_pay_routeboost5")

        status = l1.rpc.call('paystatus', [inv['bolt11']])
        assert only_one(status['pay'])['bolt11'] == inv['bolt11']
        assert only_one(status['pay'])['msatoshi'] == 10**5
        assert only_one(status['pay'])['destination'] == l5.info['id']
        assert only_one(status['pay'])['label'] == "paying test_pay_routeboost5"
        assert 'routehint_modifications' not in only_one(status['pay'])
        assert 'local_exclusions' not in only_one(status['pay'])
        attempts = only_one(status['pay'])['attempts']

        # First two failed (w/o routehint and w bad hint), third succeeded.
        assert len(attempts) == 3
        assert 'success' not in attempts[0]
        assert 'success' not in attempts[1]
        assert 'success' in attempts[2]

        assert [h['channel'] for h in attempts[1]['routehint']] == [r['short_channel_id'] for r in routel3l4l5]
        assert [h['channel'] for h in attempts[2]['routehint']] == [r['short_channel_id'] for r in routel3l5]


@flaky
@unittest.skipIf(not DEVELOPER, "gossip without DEVELOPER=1 is slow")
def test_pay_direct(node_factory, bitcoind):
    """Check that we prefer the direct route.
    """
    # l2->l3 is really cheap by comparison.
    l0, l1, l2, l3 = node_factory.get_nodes(4, opts=[{'fee-base': 1000,
                                                      'cltv-delta': 14},
                                                     {'fee-base': 1000,
                                                      'cltv-delta': 14},
                                                     {'fee-base': 0,
                                                      'cltv-delta': 14},
                                                     {'fee-base': 1000,
                                                      'cltv-delta': 14}])

    # Direct channel l0->l1->l3
    l0.rpc.connect(l1.info['id'], 'localhost', l1.port)
    # Waiting takes a *long* time if !DEVELOPER.
    c0 = l0.fund_channel(l1, 10**7, wait_for_active=False)

    l1.rpc.connect(l3.info['id'], 'localhost', l3.port)
    c1 = l1.fund_channel(l3, 10**7, wait_for_active=False)

    # Indirect route l0->l1->l2->l3
    l1.rpc.connect(l2.info['id'], 'localhost', l2.port)
    c2 = l1.fund_channel(l2, 10**7, wait_for_active=False)

    l2.rpc.connect(l3.info['id'], 'localhost', l3.port)
    c3 = l2.fund_channel(l3, 10**7, wait_for_active=False)

    # Let channels lock in.
    bitcoind.generate_block(5)

    # Make l1 sees it, so it doesn't produce bad CLTVs.
    sync_blockheight(bitcoind, [l1])

    # Make sure l0 knows the l2->l3 channel.
    # Without DEVELOPER, channel lockin can take 30 seconds to detect,
    # and gossip 2 minutes to propagate
    l0.wait_for_channel_updates([c0, c1, c2, c3])

    # Find out how much msatoshi l1 owns on l1->l2 channel.
    l1l2msatreference = only_one(l1.rpc.getpeer(l2.info['id'])['channels'])['msatoshi_to_us']

    # Try multiple times to ensure that route randomization
    # will not override our preference for direct route.
    for i in range(8):
        inv = l3.rpc.invoice(20000000, 'pay{}'.format(i), 'desc')['bolt11']

        l0.rpc.pay(inv)

        # We should have gone the direct route, so
        # l1->l2 channel msatoshi_to_us should not
        # have changed.
        l1l2msat = only_one(l1.rpc.getpeer(l2.info['id'])['channels'])['msatoshi_to_us']
        assert l1l2msat == l1l2msatreference


def test_setchannelfee_usage(node_factory, bitcoind):
    # TEST SETUP
    #
    # [l1] ---> [l2]  (channel funded)
    #   |
    #   o - - > [l3]  (only connected)
    #
    # - check initial SQL values
    # - check setchannelfee can be used
    # - checks command's return object format
    # - check custom SQL fee values
    # - check values in local nodes listchannels output
    # - json throws exception on negative values
    # - checks if peer id can be used instead of scid
    DEF_BASE = 10
    DEF_PPM = 100

    l1 = node_factory.get_node(options={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})
    l2 = node_factory.get_node(options={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})
    l3 = node_factory.get_node(options={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})
    l1.rpc.connect(l2.info['id'], 'localhost', l2.port)
    l1.rpc.connect(l3.info['id'], 'localhost', l3.port)
    l1.fund_channel(l2, 1000000)

    # get short channel id
    scid = l1.get_channel_scid(l2)
    scid_hex = scid.encode('utf-8').hex()

    # feerates should be init with global config
    db_fees = l1.db_query('SELECT feerate_base, feerate_ppm FROM channels;')
    assert(db_fees[0]['feerate_base'] == DEF_BASE)
    assert(db_fees[0]['feerate_ppm'] == DEF_PPM)

    # custom setchannelfee scid <base> <ppm>
    result = l1.rpc.setchannelfee(scid, 1337, 137)

    # check result format
    assert(result['base'] == 1337)
    assert(result['ppm'] == 137)
    assert(len(result['channels']) == 1)
    assert(re.match('^[0-9a-f]{64}$', result['channels'][0]['channel_id']))
    assert(result['channels'][0]['peer_id'] == l2.info['id'])
    assert(result['channels'][0]['short_channel_id'] == scid)

    # check if custom values made it into the database
    db_fees = l1.db_query(
        'SELECT feerate_base, feerate_ppm FROM channels '
        'WHERE hex(short_channel_id)="' + scid_hex + '";')
    assert(db_fees[0]['feerate_base'] == 1337)
    assert(db_fees[0]['feerate_ppm'] == 137)

    # wait for gossip and check if l1 sees new fees in listchannels
    wait_for(lambda: [c['base_fee_millisatoshi'] for c in l1.rpc.listchannels(scid)['channels']] == [DEF_BASE, 1337])
    wait_for(lambda: [c['fee_per_millionth'] for c in l1.rpc.listchannels(scid)['channels']] == [DEF_PPM, 137])

    # also test with named and missing paramters
    result = l1.rpc.setchannelfee(ppm=42, id=scid)
    assert(result['base'] == DEF_BASE)
    assert(result['ppm'] == 42)
    assert(len(result['channels']) == 1)
    assert(re.match('^[0-9a-f]{64}$', result['channels'][0]['channel_id']))
    assert(result['channels'][0]['short_channel_id'] == scid)
    result = l1.rpc.setchannelfee(base=42, id=scid)
    assert(result['base'] == 42)
    assert(result['ppm'] == DEF_PPM)
    assert(len(result['channels']) == 1)
    assert(re.match('^[0-9a-f]{64}$', result['channels'][0]['channel_id']))
    assert(result['channels'][0]['short_channel_id'] == scid)

    # check if negative fees raise error and DB keeps values
    # JSONRPC2_INVALID_PARAMS := -32602
    with pytest.raises(RpcError, match=r'-32602'):
        l1.rpc.setchannelfee(scid, -1, -1)

    # test if zero fees is possible
    result = l1.rpc.setchannelfee(scid, 0, 0)
    assert(result['base'] == 0)
    assert(result['ppm'] == 0)
    db_fees = l1.db_query(
        'SELECT feerate_base, feerate_ppm FROM channels '
        'WHERE hex(short_channel_id)="' + scid_hex + '";')
    assert(db_fees[0]['feerate_base'] == 0)
    assert(db_fees[0]['feerate_ppm'] == 0)

    # disable and check for global values to be returned
    result = l1.rpc.setchannelfee(scid)
    assert(result['base'] == DEF_BASE)
    assert(result['ppm'] == DEF_PPM)
    # check default values in DB
    db_fees = l1.db_query(
        'SELECT feerate_base, feerate_ppm FROM channels '
        'WHERE hex(short_channel_id)="' + scid_hex + '";')
    assert(db_fees[0]['feerate_base'] == DEF_BASE)
    assert(db_fees[0]['feerate_ppm'] == DEF_PPM)

    # check also peer id can be used
    result = l1.rpc.setchannelfee(l2.info['id'], 42, 43)
    assert(result['base'] == 42)
    assert(result['ppm'] == 43)
    assert(len(result['channels']) == 1)
    assert(result['channels'][0]['peer_id'] == l2.info['id'])
    assert(result['channels'][0]['short_channel_id'] == scid)
    db_fees = l1.db_query(
        'SELECT feerate_base, feerate_ppm FROM channels '
        'WHERE hex(short_channel_id)="' + scid_hex + '";')
    assert(db_fees[0]['feerate_base'] == 42)
    assert(db_fees[0]['feerate_ppm'] == 43)

    # check if invalid scid raises proper error
    with pytest.raises(RpcError, match=r'-1.*Could not find active channel of peer with that id'):
        result = l1.rpc.setchannelfee(l3.info['id'], 42, 43)
    with pytest.raises(RpcError, match=r'-32602.*Given id is not a channel ID or short channel ID'):
        result = l1.rpc.setchannelfee('f42' + scid[3:], 42, 43)

    # check if 'base' unit can be modified to satoshi
    result = l1.rpc.setchannelfee(scid, '1sat')
    assert(result['base'] == 1000)
    db_fees = l1.db_query(
        'SELECT feerate_base, feerate_ppm FROM channels '
        'WHERE hex(short_channel_id)="' + scid_hex + '";')
    assert(db_fees[0]['feerate_base'] == 1000)

    # check if 'ppm' values greater than u32_max fail
    with pytest.raises(RpcError, match=r'-32602.*should be an integer, not'):
        l1.rpc.setchannelfee(scid, 0, 2**32)

    # check if 'ppm' values greater than u32_max fail
    with pytest.raises(RpcError, match=r'-32602.*exceeds u32 max'):
        l1.rpc.setchannelfee(scid, 2**32)


@unittest.skipIf(not DEVELOPER, "gossip without DEVELOPER=1 is slow")
def test_setchannelfee_state(node_factory, bitcoind):
    # TEST SETUP
    #
    # [l0] --> [l1] --> [l2]
    #
    # Initiate channel [l1,l2] and try to set feerates other states than
    # CHANNELD_NORMAL or CHANNELD_AWAITING_LOCKIN. Should raise error.
    # Use l0 to make a forward through l1/l2 for testing.
    DEF_BASE = 0
    DEF_PPM = 0

    l0 = node_factory.get_node(options={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})
    l1 = node_factory.get_node(options={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})
    l2 = node_factory.get_node(options={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})

    # connection and funding
    l0.rpc.connect(l1.info['id'], 'localhost', l1.port)
    l0.fund_channel(l1, 1000000, wait_for_active=True)
    l1.rpc.connect(l2.info['id'], 'localhost', l2.port)
    scid = l1.fund_channel(l2, 1000000, wait_for_active=False)

    # try setting the fee in state AWAITING_LOCKIN should be possible
    # assert(l1.channel_state(l2) == "CHANNELD_AWAITING_LOCKIN")
    result = l1.rpc.setchannelfee(l2.info['id'], 42, 0)
    assert(result['channels'][0]['peer_id'] == l2.info['id'])
    # cid = result['channels'][0]['channel_id']

    # test routing correct new fees once routing is established
    bitcoind.generate_block(6)
    l0.wait_for_route(l2)
    inv = l2.rpc.invoice(100000, 'test_setchannelfee_state', 'desc')['bolt11']
    result = l0.rpc.pay(inv)
    assert result['status'] == 'complete'
    assert result['msatoshi_sent'] == 100042

    # Disconnect and unilaterally close from l2 to l1
    l2.rpc.disconnect(l1.info['id'], force=True)
    l1.rpc.disconnect(l2.info['id'], force=True)
    result = l2.rpc.close(scid, True, 0)
    assert result['type'] == 'unilateral'

    # wait for l1 to see unilateral close via bitcoin network
    while l1.channel_state(l2) == "CHANNELD_NORMAL":
        bitcoind.generate_block(1)
    # assert l1.channel_state(l2) == "FUNDING_SPEND_SEEN"

    # Try to setchannelfee in order to raise expected error.
    # To reduce false positive flakes, only test if state is not NORMAL anymore.
    with pytest.raises(RpcError, match=r'-1.*'):
        # l1.rpc.setchannelfee(l2.info['id'], 10, 1)
        l1.rpc.setchannelfee(l2.info['id'], 10, 1)


@unittest.skipIf(not DEVELOPER, "gossip without DEVELOPER=1 is slow")
def test_setchannelfee_routing(node_factory, bitcoind):
    # TEST SETUP
    #
    # [l1] <--default_fees--> [l2] <--specific_fees--> [l3]
    #
    # - json listchannels is able to see the new values in foreign node
    # - routing calculates fees correctly
    # - payment can be done using specific fees
    # - channel specific fees can be disabled again
    # - payment can be done using global fees
    DEF_BASE = 1
    DEF_PPM = 10

    l1, l2, l3 = node_factory.line_graph(
        3, announce_channels=True, wait_for_announce=True,
        opts={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})

    # get short channel id for 2->3
    scid = l2.get_channel_scid(l3)

    # TEST CUSTOM VALUES
    l2.rpc.setchannelfee(scid, 1337, 137)

    # wait for l1 to see updated channel via gossip
    wait_for(lambda: [c['base_fee_millisatoshi'] for c in l1.rpc.listchannels(scid)['channels']] == [1337, DEF_BASE])
    wait_for(lambda: [c['fee_per_millionth'] for c in l1.rpc.listchannels(scid)['channels']] == [137, DEF_PPM])

    # test fees are applied to HTLC forwards
    #
    # BOLT #7:
    # If l1 were to send 4,999,999 millisatoshi to l3 via l2, it needs to
    # pay l2 the fee it specified in the l2->l3 `channel_update`, calculated as
    # per [HTLC Fees](#htlc_fees):  base + amt * pm / 10**6
    #
    # 1337 + 4999999 * 137 / 1000000 = 2021.999 (2021)
    route = l1.rpc.getroute(l3.info['id'], 4999999, 1)["route"]
    assert len(route) == 2
    assert route[0]['msatoshi'] == 5002020
    assert route[1]['msatoshi'] == 4999999

    # do and check actual payment
    inv = l3.rpc.invoice(4999999, 'test_setchannelfee_1', 'desc')['bolt11']
    result = l1.rpc.pay(inv)
    assert result['status'] == 'complete'
    assert result['msatoshi_sent'] == 5002020

    # TEST DISABLE and check global fee routing
    l2.rpc.setchannelfee(scid)

    # wait for l1 to see default values again via gossip
    wait_for(lambda: [c['base_fee_millisatoshi'] for c in l1.rpc.listchannels(scid)['channels']] == [DEF_BASE, DEF_BASE])
    wait_for(lambda: [c['fee_per_millionth'] for c in l1.rpc.listchannels(scid)['channels']] == [DEF_PPM, DEF_PPM])

    # test if global fees are applied again (base 1 ppm 10)
    # 1 + 4999999 * 10 / 1000000 = 50.999 (50)
    route = l1.rpc.getroute(l3.info['id'], 4999999, 1)["route"]
    assert len(route) == 2
    assert route[0]['msatoshi'] == 5000049
    assert route[1]['msatoshi'] == 4999999

    # do and check actual payment
    inv = l3.rpc.invoice(4999999, 'test_setchannelfee_2', 'desc')['bolt11']
    result = l1.rpc.pay(inv)
    assert result['status'] == 'complete'
    assert result['msatoshi_sent'] == 5000049


@unittest.skipIf(not DEVELOPER, "gossip without DEVELOPER=1 is slow")
def test_setchannelfee_zero(node_factory, bitcoind):
    # TEST SETUP
    #
    # [l1] <--default_fees--> [l2] <--specific_fees--> [l3]
    #
    # - json listchannels is able to see the new values in foreign node
    # - routing calculates fees correctly
    # - payment can be done using zero fees
    DEF_BASE = 1
    DEF_PPM = 10

    l1, l2, l3 = node_factory.line_graph(
        3, announce_channels=True, wait_for_announce=True,
        opts={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})

    # get short channel id for 2->3
    scid = l2.get_channel_scid(l3)

    # TEST ZERO fees possible
    l2.rpc.setchannelfee(scid, 0, 0)
    wait_for(lambda: [c['base_fee_millisatoshi'] for c in l1.rpc.listchannels(scid)['channels']] == [0, DEF_BASE])
    wait_for(lambda: [c['fee_per_millionth'] for c in l1.rpc.listchannels(scid)['channels']] == [0, DEF_PPM])

    # test if zero fees are applied
    route = l1.rpc.getroute(l3.info['id'], 4999999, 1)["route"]
    assert len(route) == 2
    assert route[0]['msatoshi'] == 4999999
    assert route[1]['msatoshi'] == 4999999

    # do and check actual payment
    inv = l3.rpc.invoice(4999999, 'test_setchannelfee_3', 'desc')['bolt11']
    result = l1.rpc.pay(inv)
    assert result['status'] == 'complete'
    assert result['msatoshi_sent'] == 4999999


@unittest.skipIf(not DEVELOPER, "gossip without DEVELOPER=1 is slow")
def test_setchannelfee_restart(node_factory, bitcoind):
    # TEST SETUP
    #
    # [l1] <--default_fees--> [l2] <--specific_fees--> [l3]
    #
    # - l2 sets fees to custom values and restarts
    # - l1 routing can be made with the custom fees
    # - l2 sets fees to UIN32_MAX (db update default) and restarts
    # - l1 routing can be made to l3 and global (1 10) fees are applied
    DEF_BASE = 1
    DEF_PPM = 10
    OPTS = {'may_reconnect': True, 'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM}

    l1, l2, l3 = node_factory.line_graph(3, announce_channels=True, wait_for_announce=True, opts=OPTS)

    # get short channel idS
    scid12 = l1.get_channel_scid(l2)
    scid23 = l2.get_channel_scid(l3)

    # l2 set custom fees
    l2.rpc.setchannelfee(scid23, 1337, 137)

    # restart l2 and reconnect
    l2.restart()
    l1.rpc.connect(l2.info['id'], 'localhost', l2.port)
    l2.rpc.connect(l3.info['id'], 'localhost', l3.port)

    # Make sure l1's gossipd registered channeld activating channel.
    wait_for(lambda: [c['active'] for c in l1.rpc.listchannels(scid12)['channels']] == [True, True])

    # l1 wait for channel update from l2
    wait_for(lambda: [(c['base_fee_millisatoshi'], c['fee_per_millionth'], c['active']) for c in l1.rpc.listchannels(scid23)['channels']] == [(1337, 137, True), (DEF_BASE, DEF_PPM, True)])

    # l1 can make payment to l3 with custom fees being applied
    # Note: BOLT #7 math works out to 2021 msat fees
    inv = l3.rpc.invoice(4999999, 'test_setchannelfee_1', 'desc')['bolt11']
    result = l1.rpc.pay(inv)
    assert result['status'] == 'complete'
    assert result['msatoshi_sent'] == 5002020


def test_setchannelfee_all(node_factory, bitcoind):
    # TEST SETUP
    #
    # [l1]----> [l2]
    #   |
    #   o-----> [l3]
    DEF_BASE = 10
    DEF_PPM = 100

    l1 = node_factory.get_node(options={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})
    l2 = node_factory.get_node(options={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})
    l3 = node_factory.get_node(options={'fee-base': DEF_BASE, 'fee-per-satoshi': DEF_PPM})
    l1.rpc.connect(l2.info['id'], 'localhost', l2.port)
    l1.rpc.connect(l3.info['id'], 'localhost', l3.port)
    l1.fund_channel(l2, 1000000)
    l1.fund_channel(l3, 1000000)

    # get short channel id
    scid2 = l1.get_channel_scid(l2)
    scid3 = l1.get_channel_scid(l3)

    # now try to set all (two) channels using wildcard syntax
    result = l1.rpc.setchannelfee("all", 0xDEAD, 0xBEEF)

    wait_for(lambda: [c['base_fee_millisatoshi'] for c in l1.rpc.listchannels(scid2)['channels']] == [DEF_BASE, 0xDEAD])
    wait_for(lambda: [c['fee_per_millionth'] for c in l1.rpc.listchannels(scid2)['channels']] == [DEF_PPM, 0xBEEF])
    wait_for(lambda: [c['base_fee_millisatoshi'] for c in l1.rpc.listchannels(scid3)['channels']] == [0xDEAD, DEF_BASE])
    wait_for(lambda: [c['fee_per_millionth'] for c in l1.rpc.listchannels(scid3)['channels']] == [0xBEEF, DEF_PPM])

    assert len(result['channels']) == 2
    assert result['base'] == 0xDEAD
    assert result['ppm'] == 0xBEEF
    assert result['channels'][0]['peer_id'] == l2.info['id']
    assert result['channels'][0]['short_channel_id'] == scid2
    assert result['channels'][1]['peer_id'] == l3.info['id']
    assert result['channels'][1]['short_channel_id'] == scid3
