#pragma once
// Public test messages generated and verified with dkimpy 1.1.8.
struct DkimVector {
    const char *name, *message, *record;
};
inline const DkimVector dkim_vectors[] = {
    {"simple/simple",
     "DKIM-Signature: v=1; a=rsa-sha256; c=simple/simple; d=example.test;\r\n i=@example.test; q=dns/txt; "
     "s=fixture; t=1793986818; h=from : to :\r\n date : message-id : subject;\r\n "
     "bh=d0Q5hY7Q0GY193nI0KeOGfdlLqmyGJcMPm+Q2NcFjs0=;\r\n "
     "b=L90gIC7HS4WFRmE6TRa42zU/QnXD3DggYxKFRmBco7EQv3qp26ehPKWbxeVb7mBP5mOJ3\r\n "
     "Ng+5/0fwtKQs8VqueL2W1yvjvabQEqYfwao7Cmz/UeJdo9by6sgRPXD/1HZZjLpZDWom0lq\r\n "
     "Ku8mF32DrTGfQJOwrYXrlPj8E/sBrOZgdOWvTqmeWWEt6wki5GaUhG8orwdsVmNIUZiAJbq\r\n "
     "q41vrEXrfSP5lmGLCywdOCSyVX3ymXeqpLbg56bkjJnOkOxpUbobyc4HTZv9YKYfEl01HXd\r\n "
     "Cn9nLpzsFyXOqr+jzvetylTAT08/l+ztgp2tl7YSYsBy+onTr34V3/Ihsouw==\r\nFrom: Alice "
     "<alice@example.test>\r\nTo: recipient@elsewhere.test\r\nDate: Fri, 6 Nov 2026 12:00:00 "
     "+0000\r\nMessage-ID: <vector@example.test>\r\nSubject:  Independent\r\n\tDKIM vector\r\n\r\n  Hello\t "
     "world \t\r\n\r\n",
     "v=DKIM1; k=rsa; "
     "p=MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0Fr5NtTYhLW37rTYmwDlFO180JbDl1JfBmD5qt77aLT+/"
     "RV98b8LDcEWtgKEqLS+P9iWs39SKLPt1T/o4ZL+5fqwM4nFFTYk5dsrl8ZRGYIkB7dY87Vwg4dN8L3DWa8qzcWdZ/"
     "PwsRtrh68RH+KTjHj4SS3CzOCtTMpqLSE7RxRFgo6lCrDnjbBN9amEJ/"
     "Splx5XuC3+t9JSU652sJIwMw79oiYY7DBlW2K13d0a7BDZDFOap7c/"
     "uvqKY6HBgm7ne9kywjCtWN5HT4BiaKe543TB6uKZAkND2vjVnZw1sDz5QtgtaJiY2tWmAU4qpTxBMgsHM6+"
     "CRp9yTOAtosHiQwIDAQAB"},
    {"simple/relaxed",
     "DKIM-Signature: v=1; a=rsa-sha256; c=simple/relaxed; d=example.test;\r\n i=@example.test; q=dns/txt; "
     "s=fixture; t=1793986818; h=from : to :\r\n date : message-id : subject;\r\n "
     "bh=MEukXxL/lzZidDfvr/iVnB8hbF9WtnWtQbO1GDLPG7o=;\r\n "
     "b=veQuLixyi6UHPS7jvTH8rcPwE+jW/1B/WpteBiAlRuJzXLskTBJCsppC3nbUj8r1Qk5ZH\r\n "
     "w8yb0BQaZ/Nuo/G5tCB0q671l+gQI1UmGcAJC7ZbBC00XT80Xg9LYdhGPkHXwPl9oep0pp+\r\n "
     "P1qmq0Ikd21t5kZZA5FoBZYPaBzGHPWghOQ9ufUYJ5jZOD0wHD41rbQ5nS2ZMzkg8xl08fz\r\n "
     "fhCFLt944Bp105iGS8jgMIJmdgbkNAzkegUUa0UOWugPaXq7s1EczP4Az3Vpgn33RFpdgrr\r\n "
     "e6919/Nq8jhjyARIOOmS8MwJdfgQxmkQGz7zLhovchY8uznGRVXkzL+r+A2w==\r\nFrom: Alice "
     "<alice@example.test>\r\nTo: recipient@elsewhere.test\r\nDate: Fri, 6 Nov 2026 12:00:00 "
     "+0000\r\nMessage-ID: <vector@example.test>\r\nSubject:  Independent\r\n\tDKIM vector\r\n\r\n  Hello\t "
     "world \t\r\n\r\n",
     "v=DKIM1; k=rsa; "
     "p=MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0Fr5NtTYhLW37rTYmwDlFO180JbDl1JfBmD5qt77aLT+/"
     "RV98b8LDcEWtgKEqLS+P9iWs39SKLPt1T/o4ZL+5fqwM4nFFTYk5dsrl8ZRGYIkB7dY87Vwg4dN8L3DWa8qzcWdZ/"
     "PwsRtrh68RH+KTjHj4SS3CzOCtTMpqLSE7RxRFgo6lCrDnjbBN9amEJ/"
     "Splx5XuC3+t9JSU652sJIwMw79oiYY7DBlW2K13d0a7BDZDFOap7c/"
     "uvqKY6HBgm7ne9kywjCtWN5HT4BiaKe543TB6uKZAkND2vjVnZw1sDz5QtgtaJiY2tWmAU4qpTxBMgsHM6+"
     "CRp9yTOAtosHiQwIDAQAB"},
    {"relaxed/simple",
     "DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/simple; d=example.test;\r\n i=@example.test; q=dns/txt; "
     "s=fixture; t=1793986818; h=from : to :\r\n date : message-id : subject;\r\n "
     "bh=d0Q5hY7Q0GY193nI0KeOGfdlLqmyGJcMPm+Q2NcFjs0=;\r\n "
     "b=DG7tsHGxOO/jn9zZOAMxDIsOfVvf0SSGOLgrPtkvt2qsWtvH7AU70lWafKa9/gYlDGE+9\r\n "
     "74S2HqFld34Bo+j1dn5rePjdzQi4wSczcpvgf+34nutUNKzK2CyHc9CxhsGTRXRlj3wFOmh\r\n "
     "eA0Llc2E59FnMBCGZd6Ch0BoE9ougrMo7nVvDsxBW4ZuZq8+3xZrr3iiL1NxcdS9fXXrR3N\r\n "
     "FjD6jYpSkWa6+LKf262PfRrCIDeEVRHU5C+OS7oRhXEDVR0CC7RFPeQyV/mJq0Fr1FIgNco\r\n "
     "H4cQdsfun/vYSgrEQLzu8zEXMwGdciR2lsdOxyNq+ZrdyDZz/f9dI0EGIvIw==\r\nFrom: Alice "
     "<alice@example.test>\r\nTo: recipient@elsewhere.test\r\nDate: Fri, 6 Nov 2026 12:00:00 "
     "+0000\r\nMessage-ID: <vector@example.test>\r\nSubject:  Independent\r\n\tDKIM vector\r\n\r\n  Hello\t "
     "world \t\r\n\r\n",
     "v=DKIM1; k=rsa; "
     "p=MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0Fr5NtTYhLW37rTYmwDlFO180JbDl1JfBmD5qt77aLT+/"
     "RV98b8LDcEWtgKEqLS+P9iWs39SKLPt1T/o4ZL+5fqwM4nFFTYk5dsrl8ZRGYIkB7dY87Vwg4dN8L3DWa8qzcWdZ/"
     "PwsRtrh68RH+KTjHj4SS3CzOCtTMpqLSE7RxRFgo6lCrDnjbBN9amEJ/"
     "Splx5XuC3+t9JSU652sJIwMw79oiYY7DBlW2K13d0a7BDZDFOap7c/"
     "uvqKY6HBgm7ne9kywjCtWN5HT4BiaKe543TB6uKZAkND2vjVnZw1sDz5QtgtaJiY2tWmAU4qpTxBMgsHM6+"
     "CRp9yTOAtosHiQwIDAQAB"},
    {"relaxed/relaxed",
     "DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/relaxed; d=example.test;\r\n i=@example.test; q=dns/txt; "
     "s=fixture; t=1793986818; h=from : to :\r\n date : message-id : subject;\r\n "
     "bh=MEukXxL/lzZidDfvr/iVnB8hbF9WtnWtQbO1GDLPG7o=;\r\n "
     "b=d+qVscS0j963Py2VzQJM8gIfuCc5e1+dMOcuXrL3657m6UMCYml4LCXev1V0NC4+Orzbx\r\n "
     "UH4pS7JZmbgu1tklfPKz+eOKmBY4gDRptEBSwnlOgYgieyc6r6g/O6ZYQJrReDr6BABOw5A\r\n "
     "KltThThap+yCAXOygJAklTNyx3/6NP/9582t8uUizlNE0slJr3WXN6yqk7VQuAFAe7jKiU2\r\n "
     "SCSu+Nicjh5USdRrtFkZWXCZK4kzNBnyDgP828DCFEtphkEcBwhdpwARcf66MRY2bRTAzC2\r\n "
     "acJVgYsr+O9Nv1qG8FH1PV02L4JyeBbPFrcC5ck0ibl0IF8ckrSwKdiz6Hiw==\r\nFrom: Alice "
     "<alice@example.test>\r\nTo: recipient@elsewhere.test\r\nDate: Fri, 6 Nov 2026 12:00:00 "
     "+0000\r\nMessage-ID: <vector@example.test>\r\nSubject:  Independent\r\n\tDKIM vector\r\n\r\n  Hello\t "
     "world \t\r\n\r\n",
     "v=DKIM1; k=rsa; "
     "p=MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0Fr5NtTYhLW37rTYmwDlFO180JbDl1JfBmD5qt77aLT+/"
     "RV98b8LDcEWtgKEqLS+P9iWs39SKLPt1T/o4ZL+5fqwM4nFFTYk5dsrl8ZRGYIkB7dY87Vwg4dN8L3DWa8qzcWdZ/"
     "PwsRtrh68RH+KTjHj4SS3CzOCtTMpqLSE7RxRFgo6lCrDnjbBN9amEJ/"
     "Splx5XuC3+t9JSU652sJIwMw79oiYY7DBlW2K13d0a7BDZDFOap7c/"
     "uvqKY6HBgm7ne9kywjCtWN5HT4BiaKe543TB6uKZAkND2vjVnZw1sDz5QtgtaJiY2tWmAU4qpTxBMgsHM6+"
     "CRp9yTOAtosHiQwIDAQAB"},
};
