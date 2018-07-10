CFLAGS += -I include

all: lftpd

lftpd: lftpd.o lftpd_inet.o lftpd_string.o lftpd_log.o lftpd_io.o

test:
	make -C tests test
	
clean:
	rm -f *.o