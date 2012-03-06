This software monitors a set of directories (the "upload directories") for new
files and uploads them to a server using a persistent HTTP connection.

How to use:

1. Build a copy of bismark-data-transmit. See bismark-packages for the companion
OpenWRT package Makefile.
2. Install bismark-data-transmit on your BISmark router.
3. Create the UPLOAD_ROOT directory. By default, this is /tmp/bismark-uploads
but you can change it using the package Makefile.
4. Create subdirectories of UPLOAD_ROOT. bismark-data-transmit will monitor
these for new files to upload. Each subdirectory should represent a certain type
of data to be processed on the server side. For example, I created
`/tmp/bismark-uploads/passive` for bismark-passive log files and
`/tmp/bismark-uploads/passive-frequent` for bismark-passive's frequent
uCap-specific updates. Data from these directories will be processed by
different entities on the server side. Alternatively, you can place directory
names in /etc/config/bismark-data-transmit and they will be automatically
created before starting.
5. Restart `bismark-data-transmit` for it to rescan UPLOAD_ROOT and begin
accepting uploads from the new directories.
6. To upload files, *move* files into the subdirectories of UPLOAD_ROOT. **Do
not create new files directly inside subdirectories of UPLOAD_ROOT.** They will
not get uploaded in a timely fashion. Instead, create files somewhere else and
`mv` them into the desired subdirectory.
7. Collect your files on the server side. Files are not guaranteed to arrive in
any particular order, since files that fail to upload are retried after a delay
(3 minutes by default).
8. There is a limited buffer for storing uploads. If more than 5 MB of pending
uploads accumulate, `bismark-data-transmit` will start deleting the oldest
uploads. It counts the number of files it's deleted and writes the counters to
`/tmp/bismark-data-transmit-failures.log`.

Point 6 deserves repetition: **Do not create new files directly inside
/tmp/bismark-uploads. Instead, create the files elsewhere and `mv` them into
/tmp/bismark-uploads/<your-desired-subdirectory>.**
