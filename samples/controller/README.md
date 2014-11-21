ESP Controller Sample
===

This sample shows how to create and configure ESP controllers. The controller is in 
service.c. It registers one action that is run in response to the URI: /test/hello.

Requirements
---
* [ESP](https://embedthis.com/esp/download.html)

To run:
---
    esp

The server listens on port 4000. Browse to: 
 
     http://localhost:4000/test/hello

This then returns "Hello World" to the client.

If you modify the service.c it will be automatically recompiled and reloaded when 
next accessed.

Code:
---
* [service.c](service.c) - ESP controller source

Documentation:
---
* [ESP Documentation](https://embedthis.com/esp/doc/index.html)
* [ESP Tour](https://embedthis.com/esp/doc/start/tour.html)
* [ESP Controllers](https://embedthis.com/esp/doc/users/controllers.html)
* [ESP APIs](https://embedthis.com/esp/doc/api/esp.html)
* [ESP Guide](https://embedthis.com/esp/doc/users/index.html)
* [ESP Overview](https://embedthis.com/esp/doc/users/using.html)

See Also:
---
* [esp-html-mvc - ESP HTML MMVC](../esp-html-mvc/README.md)
* [esp-layout - ESP Layouts](../esp-layout/README.md)
* [esp-page - Serving ESP pages](../esp-page/README.md)
