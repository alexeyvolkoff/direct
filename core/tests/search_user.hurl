#User mode search

GET https://staging.onmydisk.net/search?q=Dune
authorization: token:{{token}}
HTTP 200
Content-Type: application/json
[Asserts]
body contains "Dune"
