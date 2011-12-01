This software monitors a set of directories (the "upload directories") for new
files and uploads them to a server using a persistent HTTP connection.

How to use:

1. Build a copy of bismark-data-transmit. See bismark-packages for the companion
OpenWRT package Makefile. You can customize the server URL and upload root
directory at compile time using OpenWRT's menuconfig.
2. Install bismark-data-transmit on your BISmark router. Enable
bismark-data-transmit by running `/etc/init.d/bismark-data-transmit enable &&
/etc/init.d/bismark-data-transmit start`.
3. Create the UPLOAD_ROOT directory. By default, this is /tmp/bismark-uploads
but you can change it using OpenWRT's menu system.
4. Create subdirectories of UPLOAD_ROOT. bismark-data-transmit will monitor
these for new files to upload. Each subdirectory should represent a certain type
of data to be processed on the server side. For example, I created
`/tmp/bismark-uploads/passive` for bismark-passive log files and
`/tmp/bismark-uploads/passive-frequent` for bismark-passive's frequent
uCap-specific updates. Data from these directories will be processed by
different entities on the server side.
5. Restart `bismark-data-transmit` for it to rescan UPLOAD_ROOT and begin
accepting uploads from the new directories.
6. To upload files, *move* files into the subdirectories of UPLOAD_ROOT. **Do
not create new files directly inside subdirectories of UPLOAD_ROOT.** They will
not get uploaded in a timely fashion. Instead, create files somewhere else and
`mv` them into the desired subdirectory.
7. Collect your files on the server side. Files are not guaranteed to arrive in
any particular order, since files that fail to upload are retried after a long
delay (30 minutes by default).

Point 6 deserves repetition: **Do not create new files directly inside
/tmp/bismark-uploads. Instead, create the files elsewhere and `mv` them into
/tmp/bismark-uploads/<your-desired-subdirectory>.**
