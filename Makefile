# -*- Mode: Makefile; tab-width: 4 -*-

TAR_FLAGS = --dereference --exclude RCS

dist: clean
	tar $(TAR_FLAGS) -zcvf /tmp/async.tar.gz .
	python /home/rushing/python/name_dist.py async

clean:
	rm -f *.pyc *~
