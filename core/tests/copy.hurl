#Test copy
POST https://disk.direct/signin
{
    "action": "signin",
    "username": "srdjanp",
    "password": "{{password}}"
}
HTTP 200
[Captures]
session-id: cookie "OMD_SESSION_ID"
POST https://disk.direct/
Authorization: token:{{session-id}}
Content-Type: application/json
{
    "action": "copy",
    "items": ["/Windows-desktop/Temp/Jul.JPG"],
    "newPath": "/Windows-desktop/Temp/Testing",
    "singleFilename": "Jul.JPG"
}
HTTP 200
[Asserts]
jsonpath "$.result.success" == true