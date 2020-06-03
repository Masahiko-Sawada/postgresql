--
-- Test for foreign transaction management.
--

CREATE EXTENSION test_fdwxact;

-- setup two servers that don't support transaction management API
CREATE SERVER srv_1 FOREIGN DATA WRAPPER test_fdw;
CREATE SERVER srv_2 FOREIGN DATA WRAPPER test_fdw;

-- setup two servers that support only commit and rollback API
CREATE SERVER srv_no2pc_1 FOREIGN DATA WRAPPER test_no2pc_fdw;
CREATE SERVER srv_no2pc_2 FOREIGN DATA WRAPPER test_no2pc_fdw;

-- setup two servers that support commit, rollback and prepare API.
-- That is, those two server support two-phase commit protocol.
CREATE SERVER srv_2pc_1 FOREIGN DATA WRAPPER test_2pc_fdw;
CREATE SERVER srv_2pc_2 FOREIGN DATA WRAPPER test_2pc_fdw;

CREATE TABLE t (i int);
CREATE FOREIGN TABLE ft_1 (i int) SERVER srv_1;
CREATE FOREIGN TABLE ft_2 (i int) SERVER srv_2;
CREATE FOREIGN TABLE ft_no2pc_1 (i int) SERVER srv_no2pc_1;
CREATE FOREIGN TABLE ft_no2pc_2 (i int) SERVER srv_no2pc_2;
CREATE FOREIGN TABLE ft_2pc_1 (i int) SERVER srv_2pc_1;
CREATE FOREIGN TABLE ft_2pc_2 (i int) SERVER srv_2pc_2;

CREATE USER MAPPING FOR PUBLIC SERVER srv_1;
CREATE USER MAPPING FOR PUBLIC SERVER srv_2;
CREATE USER MAPPING FOR PUBLIC SERVER srv_no2pc_1;
CREATE USER MAPPING FOR PUBLIC SERVER srv_no2pc_2;
CREATE USER MAPPING FOR PUBLIC SERVER srv_2pc_1;
CREATE USER MAPPING FOR PUBLIC SERVER srv_2pc_2;


-- Test 'disabled' mode.
-- Modifies one or two servers but since we don't require two-phase
-- commit, all case should not raise an error.
SET foreign_twophase_commit TO disabled;

BEGIN;
INSERT INTO ft_1 VALUES (1);
COMMIT;
BEGIN;
INSERT INTO ft_1 VALUES (1);
ROLLBACK;

BEGIN;
INSERT INTO ft_no2pc_1 VALUES (1);
COMMIT;
BEGIN;
INSERT INTO ft_no2pc_1 VALUES (1);
ROLLBACK;

BEGIN;
INSERT INTO ft_2pc_1 VALUES (1);
COMMIT;
BEGIN;
INSERT INTO ft_2pc_1 VALUES (1);
ROLLBACK;

BEGIN;
INSERT INTO ft_2pc_1 VALUES (1);
INSERT INTO ft_no2pc_2 VALUES (1);
COMMIT;

BEGIN;
INSERT INTO ft_2pc_1 VALUES (1);
INSERT INTO ft_2pc_2 VALUES (1);
COMMIT;


-- Test 'required' mode.
-- In this case, when two-phase commit is required, all servers
-- which are involved in the and modified need to support two-phase
-- commit protocol. Otherwise transaction will rollback.
SET foreign_twophase_commit TO 'required';

-- Ok. Writing only one server doesn't require two-phase commit.
BEGIN;
INSERT INTO ft_1 VALUES (1);
COMMIT;
BEGIN;
INSERT INTO ft_no2pc_1 VALUES (1);
COMMIT;
BEGIN;
INSERT INTO ft_2pc_1 VALUES (1);
COMMIT;

-- Ok. Writing two servers, we require two-phase commit and success.
BEGIN;
INSERT INTO ft_2pc_1 VALUES (1);
INSERT INTO ft_2pc_2 VALUES (1);
COMMIT;
BEGIN;
INSERT INTO t VALUES (1);
INSERT INTO ft_2pc_1 VALUES (1);
COMMIT;

-- Ok. Only reading servers doesn't require two-phase commit.
BEGIN;
SELECT * FROM ft_2pc_1;
SELECT * FROM ft_2pc_2;
COMMIT;
BEGIN;
SELECT * FROM ft_1;
SELECT * FROM ft_no2pc_1;
COMMIT;

-- Ok. Read one server and write one server.
BEGIN;
SELECT * FROM ft_1;
INSERT INTO ft_no2pc_1 VALUES (1);
COMMIT;
BEGIN;
SELECT * FROM ft_no2pc_1;
INSERT INTO ft_2pc_1 VALUES (1);
COMMIT;

-- Error. ft_1 doesn't support two-phase commit.
BEGIN;
INSERT INTO ft_1 VALUES (1);
INSERT INTO ft_2pc_1 VALUES (1);
COMMIT;

-- Error. ft_no2pc_1 doesn't support two-phase commit.
BEGIN;
INSERT INTO ft_no2pc_1 VALUES (1);
INSERT INTO ft_2pc_1 VALUES (1);
COMMIT;

-- Error. Both ft_1 and ft_2 don't support two-phase commit.
BEGIN;
INSERT INTO ft_1 VALUES (1);
INSERT INTO ft_2 VALUES (1);
COMMIT;

-- Error. Both ft_no2pc_1 and ft_no2pc_2 don't support two-phase
-- commit.
BEGIN;
INSERT INTO ft_no2pc_1 VALUES (1);
INSERT INTO ft_no2pc_2 VALUES (1);
COMMIT;

-- Error. Two-phase commit is required because of writes on two
-- servers: local node and ft_no2pc_1. But ft_no2pc_1 doesn't support
-- two-phase commit.
BEGIN;
INSERT INTO t VALUES (1);
INSERT INTO ft_no2pc_1 VALUES (1);
COMMIT;


-- Tests for PREPARE.
-- Prepare two transactions: local and foreign.
BEGIN;
INSERT INTO ft_2pc_1 VALUES(1);
INSERT INTO t VALUES(3);
PREPARE TRANSACTION 'global_x1';
SELECT count(*) FROM pg_foreign_xacts();
COMMIT PREPARED 'global_x1';
SELECT count(*) FROM pg_foreign_xacts();

-- Even if the transaction modified only one foreign server,
-- we prepare foreign transaction.
BEGIN;
INSERT INTO ft_2pc_1 VALUES(1);
PREPARE TRANSACTION 'global_x1';
SELECT count(*) FROM pg_foreign_xacts();
ROLLBACK PREPARED 'global_x1';
SELECT count(*) FROM pg_foreign_xacts();

-- Error. PREPARE needs all involved foreign servers to
-- support two-phsae commit.
BEGIN;
INSERT INTO ft_no2pc_1 VALUES (1);
PREPARE TRANSACTION 'global_x1';
BEGIN;
INSERT INTO ft_1 VALUES (1);
PREPARE TRANSACTION 'global_x1';

-- Error. We cannot PREPARE a distributed transaction when
-- foreign_twophase_commit is disabled.
SET foreign_twophase_commit TO 'disabled';
BEGIN;
INSERT INTO ft_2pc_1 VALUES (1);
PREPARE TRANSACTION 'global_x1';
BEGIN;
INSERT INTO ft_no2pc_1 VALUES (1);
PREPARE TRANSACTION 'global_x1';
BEGIN;
INSERT INTO ft_1 VALUES (1);
PREPARE TRANSACTION 'global_x1';


-- Test 'prefer' mode.
-- The cases where failed in 'required' mode should pass in 'prefer' mode.
-- We simply commit/rollback a transaction in one-phase on a server
-- that doesn't support two-phase commit, instead of error.
SET foreign_twophase_commit TO 'prefer';

BEGIN;
INSERT INTO ft_1 VALUES (1);
INSERT INTO ft_2pc_1 VALUES (1);
COMMIT;
BEGIN;
INSERT INTO ft_no2pc_1 VALUES (1);
INSERT INTO ft_2pc_1 VALUES (1);
COMMIT;
BEGIN;
INSERT INTO ft_1 VALUES (1);
INSERT INTO ft_2 VALUES (1);
COMMIT;
BEGIN;
INSERT INTO ft_no2pc_1 VALUES (1);
INSERT INTO ft_no2pc_2 VALUES (1);
COMMIT;
BEGIN;
INSERT INTO t VALUES (1);
INSERT INTO ft_no2pc_1 VALUES (1);
COMMIT;
