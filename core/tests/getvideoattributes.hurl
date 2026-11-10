#Test getVideoAttributes
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
    "action": "getVideoAttributes",
    "item": "/Windows-desktop/Temp/2023829_065839.mp4",
    "addToQuery": "getVideoDuration&getVideoDimensions"
}
HTTP 200
[Asserts]
jsonpath "$.result.success" isBoolean

