/* Fixture for the DataTypeCompatibility guard (Firebird 4+).
 *
 * A server with DataTypeCompatibility = 2.5 / 3.0 (firebird.conf or
 * databases.conf) describes INT128, DECFLOAT, the WITH TIME ZONE types and
 * (2.5) BOOLEAN to clients in their legacy form. The extension binds DuckDB
 * types from RDB$FIELDS, so it sends isc_dpb_set_bind with NATIVE rules on
 * every attachment; this fixture checks that the rows still arrive in the
 * declared types, with exact values.
 *
 * Load it into a database opened through a databases.conf alias that sets
 * the compatibility level (2.5 covers every coerced type), e.g.:
 *
 *   compat25 = /var/lib/firebird/data/compat25.fdb
 *   {
 *       DataTypeCompatibility = 2.5
 *   }
 *
 *   CREATE DATABASE 'compat25' USER 'SYSDBA' PASSWORD 'masterkey'
 *     DEFAULT CHARACTER SET UTF8;
 *   isql -user SYSDBA -password masterkey -i scripts/fixture_type_compat.sql compat25
 *
 * Time zones are fixed offsets, so the expected UTC values don't depend on
 * the server's time zone or its tz database.
 */

CREATE TABLE TYPE_COMPAT (
    ID    INTEGER NOT NULL PRIMARY KEY,
    I128  INT128,
    N38   NUMERIC(38, 4),
    D34   DECFLOAT(34),
    TSTZ  TIMESTAMP WITH TIME ZONE,
    TMTZ  TIME WITH TIME ZONE,
    FLAG  BOOLEAN
);

INSERT INTO TYPE_COMPAT VALUES (1,
    170141183460469231731687303715884105727,
    12345678901234567890123456789012.3456,
    1234567890123456789012345678.901234,
    TIMESTAMP '2027-01-28 07:16:01 -03:00',
    TIME '07:16:01 +02:00',
    TRUE);

INSERT INTO TYPE_COMPAT VALUES (2,
    -1000000007,
    -1.5,
    0.1,
    TIMESTAMP '2003-08-11 08:03:46 +00:00',
    TIME '23:30:00 -01:00',
    FALSE);

INSERT INTO TYPE_COMPAT VALUES (3, NULL, NULL, NULL, NULL, NULL, NULL);

COMMIT;
