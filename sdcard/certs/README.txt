cacert.pem: the root certificates the HTTPS helper trusts when it checks
web sites' certificates for the Mac's browser.

They are Mozilla's, as extracted by the curl project
(https://curl.se/docs/caextract.html), under the Mozilla Public License
2.0.  To update them, download https://curl.se/ca/cacert.pem and put it
here in place of this one.
