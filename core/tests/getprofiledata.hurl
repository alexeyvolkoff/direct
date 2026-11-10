#Test getProfileData
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
    "action": "getProfileData",
    "user": "srdjanp"
}
HTTP 200
[Asserts]
jsonpath "$.result.success" == true

