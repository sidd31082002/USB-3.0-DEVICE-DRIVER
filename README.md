# USB 3.0 Mass-Storage Driver
Host-side Linux kernel module for a Transcend USB 3.x reader (VID:PID 8564:4000).
Implements BBB bulk transfers (INQUIRY, TUR, READ CAPACITY, READ10), parses MBR partitions,
and logs SuperSpeed companions, LPM (U1/U2), and stream support.
