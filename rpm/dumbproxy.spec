Summary: Lounge Dumb Proxy
Name: lounge-dumbproxy2
Version: 2.1
Release: 3%{?dist}
URL: http://tilgovi.github.com/couchdb-lounge
License: None
Group: Lounge
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root
Provides: lounge-dumbproxy = %{version}
Obsoletes: lounge-dumbproxy < 2.1
Obsoletes: lounge-dumbproxy-transitional
Obsoletes: lounge-dumpproxy1

%description
A modified version of NGINX that handles sharding and failover of a couch cluster.

%prep
tar xvzf ../nginx_src/nginx-0.7.22.tar.gz
if [ $? -ne 0 ]; then
  exit $?
fi
cd nginx-0.7.22

%build
cd nginx-0.7.22
MODULES="--add-module=../../nginx_lounge_module"
CFLAGS="--with-cc-opt=`pkg-config --cflags json`"
LIBS="--with-ld-opt=`pkg-config --libs-only-L json`"
PREFIX="--prefix=/usr"
SBIN="--sbin-path=/usr/bin/nginx-lounge"
CONF="--conf-path=/etc/lounge/nginx/nginx.conf"
ACCESS_LOG="--http-log-path=/var/log/lounge/nginx/access.log"
ERROR_LOG="--error-log-path=/var/log/lounge/nginx/error.log"
PID="--pid-path=/var/run/lounge/nginx.pid"
LOCK="--lock-path=/var/lock/nginx-lounge.lock"
./configure $PREFIX $SBIN $CONF $ACCESS_LOG $ERROR_LOG $PID $LOCK $MODULES $CFLAGS $LIBS
if [ $? -ne 0 ]; then
  exit $?
fi
make
if [ $? -ne 0 ]; then
  exit $?
fi
cd ..

%install
cd nginx-0.7.22
make DESTDIR=$RPM_BUILD_ROOT install
if [ $? -ne 0 ]; then
  exit $?
fi
cd ../..

echo `pwd`

install -d $RPM_BUILD_ROOT/etc/lounge/nginx
install -d $RPM_BUILD_ROOT/etc/init.d
install -d $RPM_BUILD_ROOT/etc/logrotate.d
install -d $RPM_BUILD_ROOT/var/run/lounge
install -d $RPM_BUILD_ROOT/var/log/lounge
install -d $RPM_BUILD_ROOT/var/log/lounge/nginx
install -m644 conf/nginx.conf  $RPM_BUILD_ROOT/etc/lounge/nginx/nginx.conf
install -m644 conf/shards.conf $RPM_BUILD_ROOT/etc/lounge/shards.conf
install -m755 init.d/dumbproxy $RPM_BUILD_ROOT/etc/init.d/dumbproxy
install -m644 dumbproxy.logrotate $RPM_BUILD_ROOT/etc/logrotate.d/dumbproxy

%clean 
rm -Rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)

%config(noreplace) /etc/lounge/nginx/nginx.conf
%config(noreplace) /etc/lounge/shards.conf
%dir /var/log/lounge
%dir /var/log/lounge/nginx
%dir /var/run/lounge
/usr/bin/nginx-lounge
/etc/init.d/dumbproxy
/etc/logrotate.d/dumbproxy
