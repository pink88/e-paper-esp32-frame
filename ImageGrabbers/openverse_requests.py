
import requests
import json
import shutil
import re
import webbrowser

client_id = "PwJO7vH327n8bmJotSyAfWFvxmHIY0wCBh5dJwD0"
client_secret = "thLKpedbqyKtNTTxW1zcLD3b428WXV7TDhhU3EUbMiO6RlwlKOwgWDtV3ylukYQvDoGltlq85P0yBb0Lbmi4HK5eoP23c9mNkJ118q3q0c2vPZrmxEneKx0P2s5GKZap"
authtoken=""



response = requests.get("https://api.openverse.org/v1/images/?size=large,medium&excluded_source=flickr&creator=yoshitaka+amano")
#print(response.json()) 
#targetAR = round(800/600,2)
finalArr = []
i=1
for object in response.json()['results']:

    curImg = object["url"]
    finalArr.append(curImg)
    webbrowser.open_new_tab(curImg)
    # imageName = 'monet_' + str(i) + '.jpg'
    # fileName = "C:/Users/nigel/OneDrive/Pictures/ART/OV/" + imageName
    # with open(fileName, 'wb') as f:
    #     shutil.copyfileobj(res.raw, f)
    # i=i+1

print (finalArr)