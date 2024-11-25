
import * as std from "std";
import * as http from "http";
let r=http.post('http://192.168.1.26:65533/api/mod(cp)/exec_script','Accept: application/json\r\nContent-Type: application/json\r\n',
'{"script":"'+"console.log(111)"+'","src":{"client":{},"mods":["client"]}}');


console.log(r);