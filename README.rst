About
-----

Dumbproxy is an nginx module that implements the CouchDB-Lounge_ hash to proxy
key requests to backend CouchDB_ nodes. View requests and other higher level
API requests require smartproxy_ to be running as well.

The same hash function is the same as used by BigCouch_.

The lounge module depends on json-c_.

LICENSE
-------

CouchDB-Lounge (including component subprojects) are licensed under the Apache License
Please see the LICENSE_ file for details.

.. _CouchDB-Lounge: https://github.com/meebo/couchdb-lounge
.. _CouchDB: http://couchdb.apache.org/
.. _smartproxy: https://github.com/meebo/smartproxy
.. _json-c: http://oss.metaparadigm.com/json-c/
.. _LICENSE: https://github.com/meebo/dumbproxy/blob/master/LICENSE
