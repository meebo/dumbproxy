.PHONY: all clean install dumbproxy tags

all: dumbproxy

clean:
	$(MAKE) -C nginx clean

install:
	$(MAKE) -C nginx install
	mkdir -p $(DESTDIR)/etc/lounge/nginx/
	mkdir -p $(DESTDIR)/etc/init.d
	cp conf/nginx.conf $(DESTDIR)/etc/lounge/nginx/nginx.conf.dist
	cp conf/shards.conf $(DESTDIR)/etc/lounge/shards.conf.dist
	cp init.d/dumbproxy $(DESTDIR)/etc/init.d/nginx-lounge

dumbproxy:
	$(MAKE) -C nginx

rpm:
	mkdir -p build
	rpmbuild --define="_builddir build" -v -bb rpm/dumbproxy.spec
	rm -rf build

tags:
	ctags -R *
	cscope -Rb
