# Webmail nativo do Windows na porta 443

O CloudieMail usa a HTTP Server API (HTTP.sys), componente do Windows, sem
instalar IIS. O Windows recebe HTTPS e encaminha as requisições de
`webmail.<domínio>` à fila do CloudieMail. A aplicação executa o mesmo webmail,
autenticação e armazenamento Firebird.

Outros sites com hostnames distintos podem usar HTTP.sys/IIS na mesma porta.
Um processo que ocupa a porta com socket exclusivo ou uma reserva de URL
conflitante ainda impede essa configuração. O programa não substitui um
binding SNI identificado como pertencente a outro aplicativo.

## Configurar pelo programa

1. Pare o servidor e abra o CloudieMail como administrador na primeira preparação.
2. Em domínio/rede, marque **HTTPS nativo do Windows** e escolha a porta **443**.
   Novas instalações Windows já selecionam esses valores. Instalações antigas
   preservam o transporte e a porta anteriores até a alteração pelo operador.
   No painel existente, abra **Configurações → Domínio e rede**.
3. Complete DNS, certificado e primeira conta, se ainda estiverem pendentes.
   O certificado precisa incluir `mail.<domínio>` e `webmail.<domínio>` no SAN.
4. Clique em **Iniciar**. O CloudieMail importa a chave/certificado para o
   repositório do computador, registra SNI por hostname e reserva a URL para o
   usuário atual e SYSTEM. Não é necessário criar um site no IIS.

A preparação inicial e a atualização do binding após renovar o certificado
exigem administrador. Depois, o mesmo usuário pode iniciar com os registros já
preparados. Outra conta Windows pode exigir uma reserva de URL apropriada.
O programa não instala raízes de confiança nem altera configurações TLS globais;
a negociação TLS desse transporte é realizada pelo Windows/Schannel, com TLS
legado desativado no binding, e não pelo contexto OpenSSL dos protocolos de email.

Ao parar, a fila é encerrada; a reserva de URL, o binding SNI e os certificados
persistem para a próxima inicialização. Alterar domínio/porta não remove os
registros anteriores. Não há limpeza automática de certificados antigos nesta
versão. Inspecione com `netsh http show urlacl` e `netsh http show sslcert`.
O AppId do CloudieMail é `{d758f987-404e-4481-9dae-a36966a21451}`.

## Cloudflare Tunnel no mesmo computador

Para `webmail.example.com`, publique uma rota com:

| Opção | Valor |
| --- | --- |
| Hostname público | `webmail.example.com` |
| Serviço | `https://127.0.0.1:443` |
| HTTP Host Header | `webmail.example.com` |
| Origin Server Name | `webmail.example.com` |
| No TLS Verify | Desativado |
| HTTP2 Connection | Desativado (HTTP/1.1 para a origem) |
| Disable Chunked Encoding | Ativado (o webmail exige Content-Length) |

Configure o CNAME `webmail` para `<Tunnel-ID>.cfargotunnel.com`, com proxy.
Remova apenas o A/AAAA conflitante desse hostname. Não altere `mail`, MX ou os
TXT de email. O certificado da origem deve ser válido e confiável para
`webmail.example.com`; o PFX de `example.com` e `www.example.com` não cobre esse nome.
O CloudieMail precisa aceitar conexões no IP 127.0.0.1: use a escuta padrão
127.0.0.1 ou 0.0.0.0. Se escolher somente um IP LAN, use esse IP no serviço do
Tunnel. Não configure a origem como o próprio hostname público, para evitar loop.

Se o CNAME do Tunnel já estiver configurado, use a confirmação de DNS manual
no assistente: o plano automático atual propõe um A para webmail. Preserve o
CNAME existente em vez de reaplicar esse A.

Com Tunnel, as conexões locais aparecem com o IP de cloudflared. O CloudieMail
não confia automaticamente em cabeçalhos de IP encaminhado; o limite por IP
pode ser compartilhado entre visitantes desse Tunnel.

Referência: [parâmetros da origem Cloudflare](https://developers.cloudflare.com/tunnel/reference/origin-parameters/).

## Linux e validação

O Linux permanece com HTTPS por socket/OpenSSL e porta inicial 8443. Não há
HTTP.sys nem integração Apache na versão Linux.

A build MSVC e os testes do adaptador não substituem testes no Windows nativo.
A importação de chave, TLS/SNI real, coexistência com IIS e cancelamento de
requisições nativas ainda precisam dessa validação. Wine não equivale a HTTP.sys
do kernel Windows. Veja `STATUS.md` para o estado das demais verificações.

Depois de iniciar no Windows, verifique a origem preservando Host e SNI:

```powershell
curl.exe --noproxy "*" --resolve webmail.example.com:443:127.0.0.1 --max-time 15 -o NUL -w "HTTP=%{http_code}\n" https://webmail.example.com/
```

Esse teste usa a validação normal do certificado e deve retornar HTTP 200.
Execute também uma requisição a um hostname IIS existente para conferir sua
continuidade. Nenhuma dessas verificações depende de desligar o firewall.
