#Test getAttributes
POST https://staging.onmydisk.net/signin
{
    "action": "signin",
    "username": "srdjanp",
    "password": "{{password}}"
}
HTTP 200
[Captures]
session-id: cookie "OMD_SESSION_ID"
POST https://staging.onmydisk.net/
Authorization: token:{{session-id}}
Content-Type: application/json
{
    "action": "getAttributes",
    "item": "/Windows-desktop/Temp/TestingContent.txt"
}
HTTP 200
[Asserts]
#jsonpath "$.success" isBoolean

