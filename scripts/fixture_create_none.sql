/* Bootstrap database with default CHARACTER SET NONE — exercises the
 * none_encoding feature in the extension. Path is Windows-style by
 * default (matches scripts\build_windows_local.bat conventions);
 * adjust for Linux deployments.
 */
SET SQL DIALECT 3;
CREATE DATABASE 'C:\fbtest\none.fdb'
    USER 'SYSDBA' PASSWORD 'masterkey'
    DEFAULT CHARACTER SET NONE;
QUIT;
