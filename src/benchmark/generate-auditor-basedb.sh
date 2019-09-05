#!/bin/bash
# Script to generate the basic database for auditor
# testing from a 'correct' interaction between exchange,
# wallet and merchant.  Creates 'auditor-basedb.sql'.
#
# Currently must be run online as it interacts with
# bank.test.taler.net; also requires the wallet CLI
# to be installed and in the path.  Furthermore, the
# user running this script must be Postgres superuser
# and be allowed to create/drop databases.
#
set -eu

# Configuation file will be edited, so we create one
# from the template.
CONF=generate-auditor-basedb-prod.conf
cp generate-auditor-basedb-template.conf $CONF

# Name of the Postgres database we will use for the script.
# Will be dropped, do NOT use anything that might be used
# elsewhere
TARGET_DB=taler-auditor-basedb

# Clean up
DATA_DIR=`taler-config -f -c $CONF -s PATHS -o TALER_HOME`
rm -rf $DATA_DIR || true

# reset database
dropdb $TARGET_DB >/dev/null 2>/dev/null || true
createdb $TARGET_DB

# obtain key configuration data
MASTER_PRIV_FILE=`taler-config -f -c $CONF -s EXCHANGE -o MASTER_PRIV_FILE`
MASTER_PRIV_DIR=`dirname $MASTER_PRIV_FILE`
mkdir -p $MASTER_PRIV_DIR
gnunet-ecc -g1 $MASTER_PRIV_FILE > /dev/null
MASTER_PUB=`gnunet-ecc -p $MASTER_PRIV_FILE`
EXCHANGE_URL=`taler-config -c $CONF -s EXCHANGE -o BASE_URL`
MERCHANT_PORT=`taler-config -c $CONF -s MERCHANT -o PORT`
MERCHANT_URL=http://localhost:${MERCHANT_PORT}/
BANK_PORT=`taler-config -c $CONF -s BANK -o HTTP_PORT`
BANK_URL=http://localhost:${BANK_PORT}/
AUDITOR_URL=http://localhost:8888/

# patch configuration
taler-config -c $CONF -s EXCHANGE -o MASTER_PUBLIC_KEY -V $MASTER_PUB
taler-config -c $CONF -s EXCHANGE-DEFAULT -o MASTER_KEY -V $MASTER_PUB
taler-config -c $CONF -s exchangedb-postgres -o CONFIG -V postgres:///$TARGET_DB
taler-config -c $CONF -s auditordb-postgres -o CONFIG -V postgres:///$TARGET_DB
taler-config -c $CONF -s merchantdb-postgres -o CONFIG -V postgres:///$TARGET_DB
taler-config -c $CONF -s bank -o database -V postgres:///$TARGET_DB

# setup exchange
echo "Setting up exchange"
taler-exchange-dbinit -c $CONF
taler-exchange-wire -c $CONF 2> taler-exchange-wire.log
taler-exchange-keyup -c $CONF -o e2a.dat 2> taler-exchange-keyup.log

# setup auditor
echo "Setting up auditor"
taler-auditor-dbinit -c $CONF
taler-auditor-exchange -c $CONF -m $MASTER_PUB -u $EXCHANGE_URL 
taler-auditor-sign -c $CONF -u $AUDITOR_URL -r e2a.dat -o a2e.dat -m $MASTER_PUB

# Launch services
echo "Launching services"
taler-bank-manage -c $CONF serve-http &
taler-exchange-httpd -c $CONF 2> taler-exchange-httpd.log &
taler-merchant-httpd -c $CONF 2> taler-merchant-httpd.log &
taler-exchange-wirewatch -c $CONF 2> taler-exchange-wirewatch.log &

sleep 10

# run wallet CLI
echo "Running wallet"
taler-wallet-cli integrationtest -e $EXCHANGE_URL -m $MERCHANT_URL -b $BANK_URL

echo "Shutting down services"
kill `jobs -p`

# Dump database
echo "Dumping database"
pg_dump -O $TARGET_DB > auditor-basedb.sql

echo $MASTER_PUB > auditor-basedb.mpub

WIRE_FEE_DIR=`taler-config -c $CONF -f -s exchangedb -o WIREFEE_BASE_DIR`
cp $WIRE_FEE_DIR/x-taler-bank.fee auditor-basedb.fees

# clean up
echo "Final clean up"
dropdb $TARGET_DB
rm -f e2a.dat a2e.dat
rm -rf $DATA_DIR || true
rm $CONF
