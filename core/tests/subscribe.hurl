#Basic indexing test
POST https://staging.onmydisk.net
Authorization: token:{{token}}
Content-Type: application/json
{
    "action": "subscribe",
    "site": "blog"
}

HTTP 200
