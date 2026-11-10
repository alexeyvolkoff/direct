#Test move
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
    "items": ["/Windows-desktop/Temp/Testing/TestFile.JPG"],
    "newPath": "/Windows-desktop/Temp",
    "singleFilename": "TestForMove.JPG"
}
HTTP 200
[Asserts]
jsonpath "$.result.success" == true
POST https://disk.direct/
Authorization: token:{{session-id}}
Content-Type: application/json
{
    "action": "move",
    "items": ["/Windows-desktop/Temp/TestForMove.JPG"],
    "newPath": "/Windows-desktop/Temp/Testing/"
}
HTTP 200
[Asserts]
jsonpath "$.result.success" == true
