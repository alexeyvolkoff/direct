#User mode search

GET https://disk.direct/search?q=Dune
authorization: token:{{token}}
HTTP 200
Content-Type: application/json
[Asserts]
body contains "Dune"
