#!/bin/bash
# Setup database which was generated from a perfectly normal
# exchange-wallet interaction and run the auditor against it.
#
# Check that the auditor report is as expected.
#
# Requires 'jq' tool and Postgres superuser rights!
set -eu

# Set of numbers for all the testcases.
# When adding new tests, increase the last number:
ALL_TESTS=`seq 0 17`

# $TESTS determines which tests we should run.
# This construction is used to make it easy to
# only run a subset of the tests. To only run a subset,
# pass the numbers of the tests to run as the FIRST
# argument to test-auditor.sh, i.e.:
#
# $ test-auditor.sh "1 3"
#
# to run tests 1 and 3 only.  By default, all tests are run.
#
TESTS=${1:-$ALL_TESTS}

# Exit, with status code "skip" (no 'real' failure)
function exit_skip() {
    echo $1
    exit 77
}

# Exit, with error message (hard failure)
function exit_fail() {
    echo $1
    kill `jobs -p` >/dev/null 2>/dev/null || true
    exit 1
}


# Operations to run before the actual audit
function pre_audit () {
    # Launch bank
    echo -n "Launching bank "
    taler-bank-manage -c $CONF serve-http 2>bank.err >bank.log &
    for n in `seq 1 20`
    do
        echo -n "."
        sleep 0.1
        OK=1
        wget http://localhost:8082/ -o /dev/null -O /dev/null >/dev/null && break
        OK=0
    done
    if [ 1 != $OK ]
    then
        exit_skip "Failed to launch bank"
    fi
    echo " DONE"

    if test ${1:-no} = "aggregator"
    then
        echo -n "Running exchange aggregator ..."
        taler-exchange-aggregator -t -c $CONF 2> aggregator.log
        echo " DONE"
    fi
}

# actual audit run
function audit_only () {
    # Run the auditor!
    echo -n "Running audit(s) ..."
    taler-auditor -r -c $CONF -m $MASTER_PUB > test-audit.json 2> test-audit.log || exit_fail "auditor failed"
    echo -n "."
    taler-wire-auditor -r -c $CONF -m $MASTER_PUB > test-wire-audit.json 2> test-wire-audit.log || exit_fail "wire auditor failed"
    echo " DONE"
}


# Cleanup to run after the auditor
function post_audit () {
    kill -9 `jobs -p` || true

    echo -n "TeXing ."
    ../../contrib/render.py test-audit.json test-wire-audit.json < ../../contrib/auditor-report.tex.j2 > test-report.tex || exit_fail "Renderer failed"

    echo -n "."
    timeout 10 pdflatex test-report.tex >/dev/null || exit_fail "pdflatex failed"
    echo -n "."
    timeout 10 pdflatex test-report.tex >/dev/null
    echo " DONE"
}


# Run audit process on current database, including report
# generation.  Pass "aggregator" as $1 to run
# $ taler-exchange-aggregator
# before auditor (to trigger pending wire transfers).
function run_audit () {
    pre_audit ${1:-no}
    audit_only
    post_audit

}


# Do a full reload of the (original) database
full_reload()
{
    dropdb $DB 2> /dev/null || true
    createdb -T template0 $DB || exit_skip "could not create database"
    # Import pre-generated database, -q(ietly) using single (-1) transaction
    psql -Aqt $DB -q -1 -f ${BASEDB}.sql > /dev/null
}


function test_0() {

echo "===========0: normal run with aggregator==========="
run_audit aggregator

echo "Checking output"
# if an emergency was detected, that is a bug and we should fail
echo -n "Test for emergencies... "
jq -e .emergencies[0] < test-audit.json > /dev/null && exit_fail "Unexpected emergency detected in ordinary run" || echo PASS
echo -n "Test for emergencies by count... "
jq -e .emergencies_by_count[0] < test-audit.json > /dev/null && exit_fail "Unexpected emergency by count detected in ordinary run" || echo PASS

echo -n "Test for wire inconsistencies... "
jq -e .wire_out_amount_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected wire out inconsistency detected in ordinary run"
jq -e .reserve_in_amount_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected reserve in inconsistency detected in ordinary run"
jq -e .missattribution_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected missattribution inconsistency detected in ordinary run"
jq -e .row_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected row inconsistency detected in ordinary run"
jq -e .row_minor_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected minor row inconsistency detected in ordinary run"
jq -e .lag_details[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected lag detected in ordinary run"
jq -e .wire_format_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected wire format inconsistencies detected in ordinary run"

# FIXME: check operation balances are correct (once we have more transaction types)
# FIXME: check revenue summaries are correct (once we have more transaction types)

echo PASS

echo -n "Test for wire amounts... "
WIRED=`jq -r .total_wire_in_delta_plus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Expected total wire delta plus wrong, got $WIRED"
fi
WIRED=`jq -r .total_wire_in_delta_minus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Expected total wire delta minus wrong, got $WIRED"
fi
WIRED=`jq -r .total_wire_out_delta_plus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Expected total wire delta plus wrong, got $WIRED"
fi
WIRED=`jq -r .total_wire_out_delta_minus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Expected total wire delta minus wrong, got $WIRED"
fi
WIRED=`jq -r .total_missattribution_in < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Expected total missattribution in wrong, got $WIRED"
fi
echo " OK"

# FIXME: check NO lag reported

# cannot easily undo aggregator, hence full reload
echo -n "Reloading database ..."
full_reload
echo "DONE"

}


# Run without aggregator, hence auditor should detect wire
# transfer lag!
function test_1() {

echo "===========1: normal run==========="
run_audit

echo "Checking output"
# if an emergency was detected, that is a bug and we should fail
echo -n "Test for emergencies... "
jq -e .emergencies[0] < test-audit.json > /dev/null && exit_fail "Unexpected emergency detected in ordinary run" || echo PASS
echo -n "Test for emergencies by count... "
jq -e .emergencies_by_count[0] < test-audit.json > /dev/null && exit_fail "Unexpected emergency by count detected in ordinary run" || echo PASS

echo -n "Test for wire inconsistencies... "
jq -e .wire_out_amount_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected wire out inconsistency detected in ordinary run"
jq -e .reserve_in_amount_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected reserve in inconsistency detected in ordinary run"
jq -e .missattribution_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected missattribution inconsistency detected in ordinary run"
jq -e .row_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected row inconsistency detected in ordinary run"
jq -e .row_minor_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected minor row inconsistency detected in ordinary run"
jq -e .wire_format_inconsistencies[0] < test-wire-audit.json > /dev/null && exit_fail "Unexpected wire format inconsistencies detected in ordinary run"

# FIXME: check operation balances are correct (once we have more transaction types)
# FIXME: check revenue summaries are correct (once we have more transaction types)

echo PASS

echo -n "Check for lag detection... "

# Check wire transfer lag reported (no aggregator!)
# NOTE: This test is EXPECTED to fail for ~1h after
# re-generating the test database as we do not
# report lag of less than 1h (see GRACE_PERIOD in
# taler-wire-auditor.c)
AGE=`stat -c %Y ../benchmark/auditor-basedb.fees`
NOW=`date +%s`
DELTA=`expr $NOW - $AGE`
if [ $DELTA -gt 3600 ]
then
    jq -e .lag_details[0] < test-wire-audit.json > /dev/null || exit_fail "Lag not detected in run without aggregator at age $DELTA"

    LAG=`jq -r .total_amount_lag < test-wire-audit.json`
    if test $LAG = "TESTKUDOS:0"
    then
        exit_fail "Expected total lag to be non-zero"
    fi
    echo "PASS"
else
    echo "SKIP"
fi


echo -n "Test for wire amounts... "
WIRED=`jq -r .total_wire_in_delta_plus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Expected total wire delta plus wrong, got $WIRED"
fi
WIRED=`jq -r .total_wire_in_delta_minus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Expected total wire delta minus wrong, got $WIRED"
fi
WIRED=`jq -r .total_wire_out_delta_plus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Expected total wire delta plus wrong, got $WIRED"
fi
WIRED=`jq -r .total_wire_out_delta_minus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Expected total wire delta minus wrong, got $WIRED"
fi
WIRED=`jq -r .total_missattribution_in < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Expected total missattribution in wrong, got $WIRED"
fi
# Database was unmodified, no need to undo
echo "OK"
}


# Change amount of wire transfer reported by exchange
function test_2() {

echo "===========2: reserves_in inconsitency==========="
echo "UPDATE reserves_in SET credit_val=5 WHERE reserve_in_serial_id=1" | psql -Aqt $DB

run_audit

echo -n "Testing inconsistency detection... "
ROW=`jq .reserve_in_amount_inconsistencies[0].row < test-wire-audit.json`
if test $ROW != 1
then
    exit_fail "Row wrong"
fi
WIRED=`jq -r .reserve_in_amount_inconsistencies[0].amount_wired < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:10"
then
    exit_fail "Amount wrong"
fi
EXPECTED=`jq -r .reserve_in_amount_inconsistencies[0].amount_exchange_expected < test-wire-audit.json`
if test $EXPECTED != "TESTKUDOS:5"
then
    exit_fail "Expected amount wrong"
fi

WIRED=`jq -r .total_wire_in_delta_minus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Wrong total wire_in_delta_minus, got $WIRED"
fi
DELTA=`jq -r .total_wire_in_delta_plus < test-wire-audit.json`
if test $DELTA != "TESTKUDOS:5"
then
    exit_fail "Expected total wire delta plus wrong, got $DELTA"
fi
echo PASS

# Undo database modification
echo "UPDATE reserves_in SET credit_val=10 WHERE reserve_in_serial_id=1" | psql -Aqt $DB

}


# Check for incoming wire transfer amount given being
# lower than what exchange claims to have received.
function test_3() {

echo "===========3: reserves_in inconsitency==========="
echo "UPDATE reserves_in SET credit_val=15 WHERE reserve_in_serial_id=1" | psql -Aqt $DB

run_audit

EXPECTED=`jq -r .reserve_balance_summary_wrong_inconsistencies[0].auditor < test-audit.json`
if test $EXPECTED != "TESTKUDOS:5.01"
then
    exit_fail "Expected reserve balance summary amount wrong, got $EXPECTED (auditor)"
fi

EXPECTED=`jq -r .reserve_balance_summary_wrong_inconsistencies[0].exchange < test-audit.json`
if test $EXPECTED != "TESTKUDOS:0.01"
then
    exit_fail "Expected reserve balance summary amount wrong, got $EXPECTED (exchange)"
fi

WIRED=`jq -r .total_loss_balance_insufficient < test-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Wrong total loss from insufficient balance, got $WIRED"
fi

ROW=`jq -e .reserve_in_amount_inconsistencies[0].row < test-wire-audit.json`
if test $ROW != 1
then
    exit_fail "Row wrong, got $ROW"
fi

WIRED=`jq -r .reserve_in_amount_inconsistencies[0].amount_exchange_expected < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:15"
then
    exit_fail "Wrong amount_exchange_expected, got $WIRED"
fi

WIRED=`jq -r .reserve_in_amount_inconsistencies[0].amount_wired < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:10"
then
    exit_fail "Wrong amount_wired, got $WIRED"
fi

WIRED=`jq -r .total_wire_in_delta_minus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:5"
then
    exit_fail "Wrong total wire_in_delta_minus, got $WIRED"
fi

WIRED=`jq -r .total_wire_in_delta_plus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:0"
then
    exit_fail "Wrong total wire_in_delta_plus, got $WIRED"
fi

# Undo database modification
echo "UPDATE reserves_in SET credit_val=10 WHERE reserve_in_serial_id=1" | psql -Aqt $DB

}


# Check for incoming wire transfer amount given being
# lower than what exchange claims to have received.
function test_4() {

echo "===========4: deposit wire target wrong================="
# Original target bank account was 43, changing to 44
OLD_WIRE=`echo 'SELECT wire FROM deposits WHERE deposit_serial_id=1;' | psql $DB -Aqt`
echo "UPDATE deposits SET wire='{\"url\":\"payto://x-taler-bank/localhost:8082/44\",\"salt\":\"test-salt\"}' WHERE deposit_serial_id=1" | psql -Aqt $DB

run_audit

ROW=`jq -e .bad_sig_losses[0].row < test-audit.json`
if test $ROW != 1
then
    exit_fail "Row wrong, got $ROW"
fi

LOSS=`jq -r .bad_sig_losses[0].loss < test-audit.json`
if test $LOSS != "TESTKUDOS:0.1"
then
    exit_fail "Wrong deposit bad signature loss, got $LOSS"
fi

OP=`jq -r .bad_sig_losses[0].operation < test-audit.json`
if test $OP != "deposit"
then
    exit_fail "Wrong operation, got $OP"
fi

LOSS=`jq -r .total_bad_sig_loss < test-audit.json`
if test $LOSS != "TESTKUDOS:0.1"
then
    exit_fail "Wrong total bad sig loss, got $LOSS"
fi

# Undo:
echo "UPDATE deposits SET wire='$OLD_WIRE' WHERE deposit_serial_id=1" | psql -Aqt $DB

}



# Test where h_contract_terms in the deposit table is wrong
# (=> bad signature)
function test_5() {
echo "===========5: deposit contract hash wrong================="
# Modify h_wire hash, so it is inconsistent with 'wire'
OLD_H=`echo 'SELECT h_contract_terms FROM deposits WHERE deposit_serial_id=1;'  | psql $DB -Aqt`
echo "UPDATE deposits SET h_contract_terms='\x12bb676444955c98789f219148aa31899d8c354a63330624d3d143222cf3bb8b8e16f69accd5a8773127059b804c1955696bf551dd7be62719870613332aa8d5' WHERE deposit_serial_id=1" | psql -Aqt $DB

run_audit

ROW=`jq -e .bad_sig_losses[0].row < test-audit.json`
if test $ROW != 1
then
    exit_fail "Row wrong, got $ROW"
fi

LOSS=`jq -r .bad_sig_losses[0].loss < test-audit.json`
if test $LOSS != "TESTKUDOS:0.1"
then
    exit_fail "Wrong deposit bad signature loss, got $LOSS"
fi

OP=`jq -r .bad_sig_losses[0].operation < test-audit.json`
if test $OP != "deposit"
then
    exit_fail "Wrong operation, got $OP"
fi

LOSS=`jq -r .total_bad_sig_loss < test-audit.json`
if test $LOSS != "TESTKUDOS:0.1"
then
    exit_fail "Wrong total bad sig loss, got $LOSS"
fi

# Undo:
echo "UPDATE deposits SET h_contract_terms='${OLD_H}' WHERE deposit_serial_id=1" | psql -Aqt $DB

}


# Test where denom_sig in known_coins table is wrong
# (=> bad signature)
function test_6() {
echo "===========6: known_coins signature wrong================="
# Modify denom_sig, so it is wrong
OLD_SIG=`echo 'SELECT denom_sig FROM known_coins LIMIT 1;' | psql $DB -Aqt`
COIN_PUB=`echo "SELECT coin_pub FROM known_coins WHERE denom_sig='$OLD_SIG';"  | psql $DB -Aqt`
echo "UPDATE known_coins SET denom_sig='\x287369672d76616c200a2028727361200a2020287320233542383731423743393036444643303442424430453039353246413642464132463537303139374131313437353746324632323332394644443146324643333445393939413336363430334233413133324444464239413833353833464536354442374335434445304441453035374438363336434541423834463843323843344446304144363030343430413038353435363039373833434431333239393736423642433437313041324632414132414435413833303432434346314139464635394244434346374436323238344143354544364131373739463430353032323241373838423837363535453434423145443831364244353638303232413123290a2020290a20290b' WHERE coin_pub='$COIN_PUB'" | psql -Aqt $DB

run_audit

ROW=`jq -e .bad_sig_losses[0].row < test-audit.json`
if test $ROW != "-1"
then
    exit_fail "Row wrong, got $ROW"
fi

LOSS=`jq -r .bad_sig_losses[0].loss < test-audit.json`
if test $LOSS != "TESTKUDOS:0.1"
then
    exit_fail "Wrong deposit bad signature loss, got $LOSS"
fi

OP=`jq -r .bad_sig_losses[0].operation < test-audit.json`
if test $OP != "known-coin"
then
    exit_fail "Wrong operation, got $OP"
fi

LOSS=`jq -r .total_bad_sig_loss < test-audit.json`
if test $LOSS != "TESTKUDOS:0.1"
then
    exit_fail "Wrong total bad sig loss, got $LOSS"
fi

# Undo
echo "UPDATE known_coins SET denom_sig='$OLD_SIG' WHERE coin_pub='$COIN_PUB'" | psql -Aqt $DB

}



# Test where h_wire in the deposit table is wrong
function test_7() {
echo "===========7: reserves_out signature wrong================="
# Modify reserve_sig, so it is bogus
HBE=`echo 'SELECT h_blind_ev FROM reserves_out LIMIT 1;' | psql $DB -Aqt`
OLD_SIG=`echo "SELECT reserve_sig FROM reserves_out WHERE h_blind_ev='$HBE';" | psql $DB -Aqt`
A_VAL=`echo "SELECT amount_with_fee_val FROM reserves_out WHERE h_blind_ev='$HBE';" | psql $DB -Aqt`
A_FRAC=`echo "SELECT amount_with_fee_frac FROM reserves_out WHERE h_blind_ev='$HBE';" | psql $DB -Aqt`
# Normalize, we only deal with cents in this test-case
A_FRAC=`expr $A_FRAC / 1000000`
echo "UPDATE reserves_out SET reserve_sig='\x9ef381a84aff252646a157d88eded50f708b2c52b7120d5a232a5b628f9ced6d497e6652d986b581188fb014ca857fd5e765a8ccc4eb7e2ce9edcde39accaa4b' WHERE h_blind_ev='$HBE'" | psql -Aqt $DB

run_audit

OP=`jq -r .bad_sig_losses[0].operation < test-audit.json`
if test $OP != "withdraw"
then
    exit_fail "Wrong operation, got $OP"
fi

LOSS=`jq -r .bad_sig_losses[0].loss < test-audit.json`
LOSS_TOTAL=`jq -r .total_bad_sig_loss < test-audit.json`
if test $LOSS != $LOSS_TOTAL
then
    exit_fail "Expected loss $LOSS and total loss $LOSS_TOTAL do not match"
fi
if test $A_FRAC != 0
then
    if [ $A_FRAC -lt 10 ]
    then
        A_PREV="0"
    else
        A_PREV=""
    fi
    if test $LOSS != "TESTKUDOS:$A_VAL.$A_PREV$A_FRAC"
    then
        exit_fail "Expected loss TESTKUDOS:$A_VAL.$A_PREV$A_FRAC but got $LOSS"
    fi
else
    if test $LOSS != "TESTKUDOS:$A_VAL"
    then
        exit_fail "Expected loss TESTKUDOS:$A_VAL but got $LOSS"
    fi
fi

# Undo:
echo "UPDATE reserves_out SET reserve_sig='$OLD_SIG' WHERE h_blind_ev='$HBE'" | psql -Aqt $DB

}


# Test wire transfer subject disagreement!
function test_8() {

echo "===========8: wire-transfer-subject disagreement==========="
OLD_ID=`echo "SELECT id FROM app_banktransaction WHERE amount='TESTKUDOS:10.00' ORDER BY id LIMIT 1;" | psql $DB -Aqt`
OLD_WTID=`echo "SELECT subject FROM app_banktransaction WHERE id='$OLD_ID';" | psql $DB -Aqt`
NEW_WTID="CK9QBFY972KR32FVA1MW958JWACEB6XCMHHKVFMCH1A780Q12SVG"
echo "UPDATE app_banktransaction SET subject='$NEW_WTID' WHERE id='$OLD_ID';" | psql -Aqt $DB

run_audit

echo -n "Testing inconsistency detection... "
DIAG=`jq -r .reserve_in_amount_inconsistencies[0].diagnostic < test-wire-audit.json`
if test "x$DIAG" != "xwire subject does not match"
then
    exit_fail "Diagnostic wrong: $DIAG (0)"
fi
WTID=`jq -r .reserve_in_amount_inconsistencies[0].wtid < test-wire-audit.json`
if test x$WTID != x"$OLD_WTID" -a x$WTID != x"$NEW_WTID"
then
    exit_fail "WTID reported wrong: $WTID"
fi
EX_A=`jq -r .reserve_in_amount_inconsistencies[0].amount_exchange_expected < test-wire-audit.json`
if test x$WTID = x$OLD_WTID -a x$EX_A != x"TESTKUDOS:10"
then
    exit_fail "Amount reported wrong: $EX_A"
fi
if test x$WTID = x$NEW_WTID -a x$EX_A != x"TESTKUDOS:0"
then
    exit_fail "Amount reported wrong: $EX_A"
fi
DIAG=`jq -r .reserve_in_amount_inconsistencies[1].diagnostic < test-wire-audit.json`
if test "x$DIAG" != "xwire subject does not match"
then
    exit_fail "Diagnostic wrong: $DIAG (1)"
fi
WTID=`jq -r .reserve_in_amount_inconsistencies[1].wtid < test-wire-audit.json`
if test $WTID != "$OLD_WTID" -a $WTID != "$NEW_WTID"
then
    exit_fail "WTID reported wrong: $WTID"
fi
EX_A=`jq -r .reserve_in_amount_inconsistencies[1].amount_exchange_expected < test-wire-audit.json`
if test $WTID = "$OLD_WTID" -a $EX_A != "TESTKUDOS:10"
then
    exit_fail "Amount reported wrong: $EX_A"
fi
if test $WTID = "$NEW_WTID" -a $EX_A != "TESTKUDOS:0"
then
    exit_fail "Amount reported wrong: $EX_A"
fi

WIRED=`jq -r .total_wire_in_delta_minus < test-wire-audit.json`
if test $WIRED != "TESTKUDOS:10"
then
    exit_fail "Wrong total wire_in_delta_minus, got $WIRED"
fi
DELTA=`jq -r .total_wire_in_delta_plus < test-wire-audit.json`
if test $DELTA != "TESTKUDOS:10"
then
    exit_fail "Expected total wire delta plus wrong, got $DELTA"
fi
echo PASS

# Undo database modification
echo "UPDATE app_banktransaction SET subject='$OLD_WTID' WHERE id='$OLD_ID';" | psql -Aqt $DB

}



# Test wire origin disagreement!
function test_9() {

echo "===========9: wire-origin disagreement==========="
OLD_ID=`echo "SELECT id FROM app_banktransaction WHERE amount='TESTKUDOS:10.00' ORDER BY id LIMIT 1;" | psql $DB -Aqt`
OLD_ACC=`echo "SELECT debit_account_id FROM app_banktransaction WHERE id='$OLD_ID';" | psql $DB -Aqt`
echo "UPDATE app_banktransaction SET debit_account_id=1;" | psql -Aqt $DB

run_audit

echo -n "Testing inconsistency detection... "
AMOUNT=`jq -r .missattribution_in_inconsistencies[0].amount < test-wire-audit.json`
if test "x$AMOUNT" != "xTESTKUDOS:10"
then
    exit_fail "Reported amount wrong: $AMOUNT"
fi
AMOUNT=`jq -r .total_missattribution_in < test-wire-audit.json`
if test "x$AMOUNT" != "xTESTKUDOS:10"
then
    exit_fail "Reported total amount wrong: $AMOUNT"
fi
echo PASS

# Undo database modification
echo "UPDATE app_banktransaction SET debit_account_id=$OLD_ACC;" | psql -Aqt $DB

}


# Test wire_in timestamp disagreement!
function test_10() {

echo "===========10: wire-timestamp disagreement==========="
OLD_ID=`echo "SELECT id FROM app_banktransaction WHERE amount='TESTKUDOS:10.00' ORDER BY id LIMIT 1;" | psql $DB -Aqt`
OLD_DATE=`echo "SELECT date FROM app_banktransaction WHERE id='$OLD_ID';" | psql $DB -Aqt`
echo "UPDATE app_banktransaction SET date=NOW() WHERE id=$OLD_ID;" | psql -Aqt $DB

run_audit

echo -n "Testing inconsistency detection... "
DIAG=`jq -r .row_minor_inconsistencies[0].diagnostic < test-wire-audit.json`
if test "x$DIAG" != "xexecution date missmatch"
then
    exit_fail "Reported diagnostic wrong: $DIAG"
fi
TABLE=`jq -r .row_minor_inconsistencies[0].table < test-wire-audit.json`
if test "x$TABLE" != "xreserves_in"
then
    exit_fail "Reported table wrong: $TABLE"
fi
echo PASS

# Undo database modification
echo "UPDATE app_banktransaction SET date='$OLD_DATE' WHERE id=$OLD_ID;" | psql -Aqt $DB

}


# Test for extra outgoing wire transfer.
function test_11() {

echo "===========11: spurious outgoing transfer ==========="
OLD_ID=`echo "SELECT id FROM app_banktransaction WHERE amount='TESTKUDOS:10.00' ORDER BY id LIMIT 1;" | psql $DB -Aqt`
OLD_ACC=`echo "SELECT debit_account_id FROM app_banktransaction WHERE id=$OLD_ID;" | psql $DB -Aqt`
# Change wire transfer to be FROM the exchange (#2) to elsewhere!
# (Note: this change also causes a missing incoming wire transfer, but
#  this test is only concerned about the outgoing wire transfer
#  being detected as such, and we simply ignore the other
#  errors being reported.)
echo "UPDATE app_banktransaction SET debit_account_id=2,credit_account_id=1 WHERE id=$OLD_ID;" | psql -Aqt $DB

run_audit

echo -n "Testing inconsistency detection... "
AMOUNT=`jq -r .wire_out_amount_inconsistencies[0].amount_wired < test-wire-audit.json`
if test "x$AMOUNT" != "xTESTKUDOS:10"
then
    exit_fail "Reported wired amount wrong: $AMOUNT"
fi
AMOUNT=`jq -r .total_wire_out_delta_plus < test-wire-audit.json`
if test "x$AMOUNT" != "xTESTKUDOS:10"
then
    exit_fail "Reported total plus amount wrong: $AMOUNT"
fi
AMOUNT=`jq -r .total_wire_out_delta_minus < test-wire-audit.json`
if test "x$AMOUNT" != "xTESTKUDOS:0"
then
    exit_fail "Reported total minus amount wrong: $AMOUNT"
fi
AMOUNT=`jq -r .wire_out_amount_inconsistencies[0].amount_justified < test-wire-audit.json`
if test "x$AMOUNT" != "xTESTKUDOS:0"
then
    exit_fail "Reported justified amount wrong: $AMOUNT"
fi
DIAG=`jq -r .wire_out_amount_inconsistencies[0].diagnostic < test-wire-audit.json`
if test "x$DIAG" != "xjustification for wire transfer not found"
then
    exit_fail "Reported diagnostic wrong: $DIAG"
fi
echo PASS

# Undo database modification (exchange always has account #2)
echo "UPDATE app_banktransaction SET debit_account_id=$OLD_ACC,credit_account_id=2 WHERE id=$OLD_ID;" | psql -Aqt $DB

}



# Test for hanging/pending refresh.
function test_12() {

echo "===========12: incomplete refresh ==========="
OLD_ACC=`echo "DELETE FROM refresh_revealed_coins;" | psql $DB -Aqt`

run_audit

echo -n "Testing hung refresh detection... "

HANG=`jq -er .refresh_hanging[0].amount < test-audit.json`
TOTAL_HANG=`jq -er .total_refresh_hanging < test-audit.json`
if test x$HANG != x$TOTAL_HANG
then
    exit_fail "Hanging amount inconsistent, got $HANG and $TOTAL_HANG"
fi
if test x$TOTAL_HANG = TESTKUDOS:0
then
    exit_fail "Hanging amount zero"
fi

echo PASS


# cannot easily undo DELETE, hence full reload
echo -n "Reloading database ..."
full_reload
echo "DONE"
}


# Test for wrong signature on refresh.
function test_13() {

echo "===========13: wrong melt signature ==========="
# Modify denom_sig, so it is wrong
COIN_PUB=`echo "SELECT old_coin_pub FROM refresh_commitments LIMIT 1;"  | psql $DB -Aqt`
OLD_SIG=`echo "SELECT old_coin_sig FROM refresh_commitments WHERE old_coin_pub='$COIN_PUB';" | psql $DB -Aqt`
NEW_SIG="\xba588af7c13c477dca1ac458f65cc484db8fba53b969b873f4353ecbd815e6b4c03f42c0cb63a2b609c2d726e612fd8e0c084906a41f409b6a23a08a83c89a02"
echo "UPDATE refresh_commitments SET old_coin_sig='$NEW_SIG' WHERE old_coin_pub='$COIN_PUB'" | psql -Aqt $DB

run_audit

echo -n "Testing inconsistency detection... "

OP=`jq -er .bad_sig_losses[0].operation < test-audit.json`
if test x$OP != xmelt
then
    exit_fail "Operation wrong, got $OP"
fi

LOSS=`jq -er .bad_sig_losses[0].loss < test-audit.json`
TOTAL_LOSS=`jq -er .total_bad_sig_loss < test-audit.json`
if test x$LOSS != x$TOTAL_LOSS
then
    exit_fail "Loss inconsistent, got $LOSS and $TOTAL_LOSS"
fi
if test x$TOTAL_LOSS = TESTKUDOS:0
then
    exit_fail "Loss zero"
fi

echo PASS

# cannot easily undo DELETE, hence full reload
echo -n "Reloading database ..."
full_reload
echo "DONE"
}


# Test for wire fee disagreement
function test_14() {

echo "===========14: wire-fee disagreement==========="


# Wire fees are only checked/generated once there are
# actual outgoing wire transfers, so we need to run the
# aggregator here.
pre_audit aggregator
echo "UPDATE wire_fee SET wire_fee_frac=100;" | psql -Aqt $DB
audit_only
post_audit

echo -n "Testing inconsistency detection... "
TABLE=`jq -r .row_inconsistencies[0].table < test-audit.json`
if test "x$TABLE" != "xwire-fee"
then
    exit_fail "Reported table wrong: $TABLE"
fi
DIAG=`jq -r .row_inconsistencies[0].diagnostic < test-audit.json`
if test "x$DIAG" != "xwire fee signature invalid at given time"
then
    exit_fail "Reported diagnostic wrong: $DIAG"
fi
echo PASS

# cannot easily undo aggregator, hence full reload
echo -n "Reloading database ..."
full_reload
echo "DONE"

}



# Test where h_wire in the deposit table is wrong
function test_15() {
echo "===========15: deposit wire hash wrong================="
# Modify h_wire hash, so it is inconsistent with 'wire'
echo "UPDATE deposits SET h_wire='\x973e52d193a357940be9ef2939c19b0575ee1101f52188c3c01d9005b7d755c397e92624f09cfa709104b3b65605fe5130c90d7e1b7ee30f8fc570f39c16b853' WHERE deposit_serial_id=1" | psql -Aqt $DB

# The auditor checks h_wire consistency only for
# coins where the wire transfer has happened, hence
# run aggregator first to get this test to work.
run_audit aggregator

echo -n "Testing inconsistency detection... "
TABLE=`jq -r .row_inconsistencies[0].table < test-audit.json`
if test "x$TABLE" != "xaggregation" -a "x$TABLE" != "xdeposits"
then
    exit_fail "Reported table wrong: $TABLE"
fi
echo PASS

# cannot easily undo aggregator, hence full reload
echo -n "Reloading database ..."
full_reload
echo "DONE"
}


# Test where wired amount (wire out) is wrong
function test_16() {
echo "===========16: incorrect wire_out amount================="

# First, we need to run the aggregator so we even
# have a wire_out to modify.
pre_audit aggregator

# Modify wire amount, such that it is inconsistent with 'aggregation'
# (exchange account is #2, so the logic below should select the outgoing
# wire transfer):
OLD_ID=`echo "SELECT id FROM app_banktransaction WHERE debit_account_id=2 ORDER BY id LIMIT 1;" | psql $DB -Aqt`
OLD_AMOUNT=`echo "SELECT amount FROM app_banktransaction WHERE id='${OLD_ID}';" | psql $DB -Aqt`
NEW_AMOUNT="TESTKUDOS:50"
echo "UPDATE app_banktransaction SET amount='${NEW_AMOUNT}' WHERE id='${OLD_ID}';" | psql -Aqt $DB

audit_only

echo -n "Testing inconsistency detection... "

AMOUNT=`jq -r .wire_out_amount_inconsistencies[0].amount_justified < test-wire-audit.json`
if test "x$AMOUNT" != "x$OLD_AMOUNT"
then
    exit_fail "Reported justified amount wrong: $AMOUNT"
fi
AMOUNT=`jq -r .wire_out_amount_inconsistencies[0].amount_wired < test-wire-audit.json`
if test "x$AMOUNT" != "x$NEW_AMOUNT"
then
    exit_fail "Reported wired amount wrong: $AMOUNT"
fi
TOTAL_AMOUNT=`jq -r .total_wire_out_delta_minus < test-wire-audit.json`
if test "x$TOTAL_AMOUNT" != "xTESTKUDOS:0"
then
    exit_fail "Reported total wired amount minus wrong: $TOTAL_AMOUNT"
fi
TOTAL_AMOUNT=`jq -r .total_wire_out_delta_plus < test-wire-audit.json`
if test "x$TOTAL_AMOUNT" = "xTESTKUDOS:0"
then
    exit_fail "Reported total wired amount plus wrong: $TOTAL_AMOUNT"
fi
echo PASS

echo "Second modification: wire nothing"
NEW_AMOUNT="TESTKUDOS:0"
echo "UPDATE app_banktransaction SET amount='${NEW_AMOUNT}' WHERE id='${OLD_ID}';" | psql -Aqt $DB

audit_only

echo -n "Testing inconsistency detection... "

AMOUNT=`jq -r .wire_out_amount_inconsistencies[0].amount_justified < test-wire-audit.json`
if test "x$AMOUNT" != "x$OLD_AMOUNT"
then
    exit_fail "Reported justified amount wrong: $AMOUNT"
fi
AMOUNT=`jq -r .wire_out_amount_inconsistencies[0].amount_wired < test-wire-audit.json`
if test "x$AMOUNT" != "x$NEW_AMOUNT"
then
    exit_fail "Reported wired amount wrong: $AMOUNT"
fi
TOTAL_AMOUNT=`jq -r .total_wire_out_delta_minus < test-wire-audit.json`
if test "x$TOTAL_AMOUNT" != "x$OLD_AMOUNT"
then
    exit_fail "Reported total wired amount minus wrong: $TOTAL_AMOUNT (wanted $OLD_AMOUNT)"
fi
TOTAL_AMOUNT=`jq -r .total_wire_out_delta_plus < test-wire-audit.json`
if test "x$TOTAL_AMOUNT" != "xTESTKUDOS:0"
then
    exit_fail "Reported total wired amount plus wrong: $TOTAL_AMOUNT"
fi
echo PASS

post_audit


# Undo
echo "UPDATE app_banktransaction SET amount='${OLD_AMOUNT}' WHERE id='${OLD_ID}';" | psql -Aqt $DB
}




# Test where wire-out timestamp is wrong
function test_17() {
echo "===========17: incorrect wire_out timestamp================="

# First, we need to run the aggregator so we even
# have a wire_out to modify.
pre_audit aggregator

# Modify wire amount, such that it is inconsistent with 'aggregation'
# (exchange account is #2, so the logic below should select the outgoing
# wire transfer):
OLD_ID=`echo "SELECT id FROM app_banktransaction WHERE debit_account_id=2 ORDER BY id LIMIT 1;" | psql $DB -Aqt`
OLD_DATE=`echo "SELECT date FROM app_banktransaction WHERE id='${OLD_ID}';" | psql $DB -Aqt`
# Note: need - interval '1h' as "NOW()" may otherwise be exactly what is already in the DB
# (due to rounding, if this machine is fast...)
echo "UPDATE app_banktransaction SET date=NOW()- interval '1 hour' WHERE id='${OLD_ID}';" | psql -Aqt $DB

audit_only
post_audit

echo -n "Testing inconsistency detection... "
TABLE=`jq -r .row_minor_inconsistencies[0].table < test-wire-audit.json`
if test "x$TABLE" != "xwire_out"
then
    exit_fail "Reported table wrong: $TABLE"
fi
DIAG=`jq -r .row_minor_inconsistencies[0].diagnostic < test-wire-audit.json`
DIAG=`echo "$DIAG" | awk '{print $1 " " $2 " " $3}'`
if test "x$DIAG" != "xexecution date missmatch"
then
    exit_fail "Reported diagnostic wrong: $DIAG"
fi
echo PASS

# Undo
echo "UPDATE app_banktransaction SET date='${OLD_DATE}' WHERE id='${OLD_ID}';" | psql -Aqt $DB
}




# **************************************************
# FIXME: Add more tests here! :-)
# Specifically:
# - emergencies (detection)
# - revocation (payback, accepting
#   of coins despite denomination revocation)
# - refunds
# - reserve closure (or lack thereof)
# - arithmetic problems
# - wire.row_inconsistencies (i.e. duplicate wire offset)
# **************************************************



# *************** Main logic starts here **************

# ####### Setup globals ######
# Postgres database to use
DB=taler-auditor-test
# Prefix for the data resources to use
BASEDB="../benchmark/auditor-basedb"
MASTER_PUB=`cat ${BASEDB}.mpub`
# Configuration file to use
CONF=test-auditor.conf

# Where to store wire fee details for aggregator
WIRE_FEE_DIR=`taler-config -c $CONF -f -s exchangedb -o WIREFEE_BASE_DIR`
mkdir -p $WIRE_FEE_DIR
cp ${BASEDB}.fees $WIRE_FEE_DIR/x-taler-bank.fee


# test required commands exist
echo "Testing for jq"
jq -h > /dev/null || exit_skip "jq required"
echo "Testing for taler-bank-manage"
taler-bank-manage -h >/dev/null </dev/null || exit_skip "taler-bank-manage required"
echo "Testing for pdflatex"
which pdflatex > /dev/null </dev/null || exit_skip "pdflatex required"

echo -n "Database setup ..."
full_reload
echo " DONE"

# Run test suite
fail=0
for i in $TESTS
do
    test_$i
    if test 0 != $fail
    then
       break
    fi
done


echo "Cleanup (disabled)"
# dropdb $DB
# rm -r $WIRE_FEE_DIR
# rm -f test-audit.log test-wire-audit.log

exit $fail
