XDBARDDRV 0.9.7 was withdrawn on 2026-11-20.

With this version every query on a Microsoft SQL Server fails with SQLSTATE 07008, SQLCODE -438: the driver took the row status column that SQL Server adds to the rows of a server cursor for a column of the statement. PostgreSQL and MySQL connections do not have this defect.

Fixed in 0.9.9, which also reads decimal columns of more than 31 digits (the SUM of a decimal column on SQL Server and MySQL) and tells a rollback by the server at once.

The two save files of this version were removed from this folder and from its release. The other files stay as the record of the build (SHA256SUMS still lists the save files), and the sources are at tag v0.9.7.
