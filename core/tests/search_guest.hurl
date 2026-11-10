#Simple search test in quest mode
GET https://staging.onmydisk.net/search?q=Dune
HTTP 200
Content-Type: application/json
[Asserts]
body contains "Dune"
