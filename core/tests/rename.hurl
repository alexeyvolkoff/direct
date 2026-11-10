#Test rename
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
    "singleFilename": "TestForRename.JPG"
}
HTTP 200
[Asserts]
jsonpath "$.result.success" == true
POST https://disk.direct/
Authorization: token:{{session-id}}
Content-Type: application/json
{
    "action": "rename",
    "item": "/Windows-desktop/Temp/TestForRename.JPG",
    "newItemName": "TestRenamed.JPG"
}
HTTP 200
[Asserts]
jsonpath "$.result.success" == true

