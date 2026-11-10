#Simple search test in quest mode
GET https://disk.direct/search?q=Dune
HTTP 200
Content-Type: application/json
[Asserts]
body contains "Dune"
