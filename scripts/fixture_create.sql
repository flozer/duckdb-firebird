/* Bootstrap the biz4 database used by fixture_biz4.sql.
 * Run via:  isql -i fixture_create.sql
 *
 * On Windows the default path is C:\fbtest\biz4.fdb; change it below to
 * suit your machine.
 */
SET SQL DIALECT 3;
CREATE DATABASE 'C:\fbtest\biz4.fdb'
    USER 'SYSDBA' PASSWORD 'masterkey'
    DEFAULT CHARACTER SET UTF8;
QUIT;
