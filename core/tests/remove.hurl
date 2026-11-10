#Test remove
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
    "action": "copy",
    "items": ["/Windows-desktop/Temp/Testing/TestFile.JPG"],
    "newPath": "/Windows-desktop/Temp",
    "singleFilename": "TestForRemove.JPG"
}
HTTP 200
[Asserts]
jsonpath "$.result.success" == true
POST https://staging.onmydisk.net/
Authorization: token:{{session-id}}
Content-Type: application/json
{
    "action": "remove",
    "items": ["/Windows-desktop/Temp/TestForRemove.JPG"],
    "forced": false
}
HTTP 200
[Asserts]
jsonpath "$.result.success" == true
