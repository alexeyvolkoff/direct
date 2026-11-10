openssl req -new -sha256 -nodes -out onmydisk.local.csr -newkey rsa:2048 -keyout onmydisk.local.key -config <(
cat <<-EOF
[req]
default_bits = 2048
prompt = no
default_md = sha256
req_extensions = req_ext
distinguished_name = dn

[ dn ]
C=SI
L=Ljubljana
emailAddress=admin@onmydisk.com
CN = onmydisk.local

[ req_ext ]
subjectAltName = @alt_names

[ alt_names ]
DNS.1 = onmydisk.local
DNS.2 = public.onmydisk.local
EOF
)
