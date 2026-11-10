#Test list
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
    "action": "list",
    "path": "/Windows-desktop/Berlin/"
}
HTTP 200
Content-Type: text/json; charset=UTF-8
[Asserts]
jsonpath "$.folder.customAttributes.title" == "Berlin"
jsonpath "$.folder.owner" == "srdjanp"

