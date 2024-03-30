#!/bin/dev python

import os
import sys
import argparse
import requests

def upload(path, url):
    headers = {
        "X-Flash-Mode": "firmware",
        "Content-Type": "application/octet-stream",
    }

    with open(path, "rb") as f:
        print("Uploading %s to %s..." % (path, url))
        r = requests.post(url, headers=headers, data=f)
        print("Upload complete! %s" % r.text)

parser = argparse.ArgumentParser(description='Upload firmware to to OTA target')

parser.add_argument('-u', '--url', required=True, help="URL on where to upload the firmware.bin")
parser.add_argument('firmware', help="Path to the firmware.bin that should be uploaded")

args = parser.parse_args()

url = args.url
firmware = args.firmware

if not os.path.isfile(firmware):
    sys.exit("Firmare file %s does not exists." % firmware)

upload(firmware, url)