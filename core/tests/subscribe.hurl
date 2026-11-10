#Basic indexing test
POST https://disk.direct
Authorization: token:{{token}}
Content-Type: application/json
{
    "action": "subscribe",
    "site": "blog"
}

HTTP 200
