all: lftpd

lftpd: lftpd.o lftpd_inet.o lftpd_string.o lftpd_log.o lftpd_io.o

clean:
	rm -f *.o