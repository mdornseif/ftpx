CC = gcc
CFLAGS = -Wall -g
LIBS= -lz

ftpx:	ftpx.c tiger.o ftpparse.o ubi_sLinkList.o ubi_BinTree.o dns.a env.a byte.a alloc.a unix.a buffer.a djb.a
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

djb.a: slurp.o slurpclose.o error.o error_str.o iopause.o open_read.o tai_add.o tai_now.o tai_pack.o tai_uint.o tai_unpack.o taia_approx.o taia_frac.o taia_less.o taia_now.o taia_pack.o taia_sub.o taia_tai.o taia_uint.o taia_add.o tai_sub.o
	ar cr $@ $^
	ranlib $@	

#dns.a: dns_dfd.o dns_domain.o dns_dtda.o dns_packet.o dns_random.o dns_sortip.o dns_transmit.o dns_resolve.o dns_rcip.o dns_ip.o dns_ipq.o dns_rcrw.o dns_mx.o dns_txt.o dns_nd.o dns_name.o
#	ar cr $@ $^
#	ranlib $@

clean:
	rm -f *.o ftpx core
