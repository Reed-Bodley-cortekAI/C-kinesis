#!/bin/bash
# You need to setup your aws cli first, because this script is based on aws cli.
# You can use this script to setup environment variables in the shell which samples run on.
# https://docs.aws.amazon.com/kinesisvideostreams/latest/dg/how-iot.html
thingName="reed_mac_pc"
thingTypeName="webrtc_iot_thing_type"
iotPolicyName="webrtc_iot_policy"
kvsPolicyName="webrtc_policy"
iotRoleName="webrtc_iot_role"
iotRoleAlias="webrtc_iot_role_alias"
iotCert="/Users/reedbodley/Downloads/connect_device_package/reed_mac_pc.cert.pem"
iotPublicKey="webrtc_iot_public.key"
iotPrivateKey="/Users/reedbodley/Downloads/connect_device_package/reed_mac_pc.private.key"



export AWS_IOT_CORE_CREDENTIAL_ENDPOINT=$(cat iot-credential-provider.txt)
export AWS_IOT_CORE_CERT="/Users/reedbodley/Downloads/connect_device_package/reed_mac_pc.cert.pem"
export AWS_IOT_CORE_PRIVATE_KEY="/Users/reedbodley/Downloads/connect_device_package/reed_mac_pc.private.key"
export AWS_IOT_CORE_ROLE_ALIAS=$iotRoleAlias
export AWS_IOT_CORE_THING_NAME=$thingName
export AWS_DEFAULT_REGION="us-east-1"