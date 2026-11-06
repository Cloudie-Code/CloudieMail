# Configuração

## Assistente

Abra `CloudieMail.sh` no Linux ou `CloudieMail.cmd` no Windows. O assistente configura domínio, rede, DNS, certificado e primeira conta. O servidor permanece parado até você clicar em **Iniciar**.

As contas usam o domínio principal; o servidor usa `mail.<domínio>` e o webmail usa `webmail.<domínio>`. Nas telas de configuração é possível ajustar portas e limites, criar contas, trocar senhas e emitir ou importar certificados.

No terminal, execute `Console.sh` ou `Console.cmd`. Para reabrir as configurações de uma instalação existente, pare o servidor e use:

```text
Console.cmd configure config/local.json
```

No Linux, substitua o lançador por `./Console.sh`.

## Portas

| Serviço | Porta pública usual | Padrão do assistente |
| --- | --- | --- |
| SMTP entre servidores | 25 | 2525 |
| SMTP com TLS implícito | 465 | 2465 |
| Submissão com STARTTLS | 587 | 2587 |
| IMAP com TLS | 993 | 2993 |
| POP3 com TLS | 995 | 2995 |
| HTTPS do webmail | 443 | Windows: 443; Linux independente: 8443 |

Os valores altos facilitam testes locais. Para clientes de email e recebimento público, configure as portas públicas ou encaminhe-as para as portas do serviço. No Linux, portas abaixo de 1024 exigem permissão de bind apropriada. Não execute todo o servidor como root apenas para contornar essa restrição.

O firewall precisa permitir os serviços aos clientes. Restringir 80/443 aos IPs do Cloudflare não deve restringir IMAP/SMTP a esses IPs. A entrega externa precisa de **saída** TCP 25; encaminhamento de entrada não libera essa saída.

## DNS e Cloudflare

A tela de DNS apresenta A, MX, SPF, DKIM e DMARC. `mail` deve ficar sem proxy HTTP. O plano inicial também cria `webmail` sem proxy; configurações com Tunnel ou proxy próprio precisam preservar sua rota existente.

A API requer um token restrito à zona com permissão de leitura da zona e edição de DNS. O programa cria registros ausentes e recusa conflitos. Revise a lista antes de aplicar; uma operação interrompida pode deixar parte dos registros criada.

O token não é salvo na configuração. Ele é solicitado novamente quando necessário.

### DKIM

Abra **Configurações → Registros DNS** e localize `cloudiemail._domainkey.<domínio>`. Publique um TXT com o conteúdo completo iniciado por `v=DKIM1; k=rsa; p=`. Se o formato de zona mostrar vários trechos entre aspas, concatene os conteúdos sem inserir espaço entre eles.

Publique somente o registro público exibido. O arquivo `data/dkim.json` contém a chave privada do servidor e deve permanecer protegido.

O CloudieMail assina as mensagens de saída automaticamente. Após publicar o TXT, envie uma nova mensagem e confira os resultados de SPF, DKIM e DMARC nos cabeçalhos do destinatário.

### PTR e IPv6

O provedor do endereço IP configura o PTR. O nome indicado pelo PTR deve resolver de volta ao IP de saída. O token da zona do domínio no Cloudflare normalmente não permite alterar esse reverso.

A entrega externa usa IPv4 por padrão. **Permitir envio por IPv6**, em Preferências e limites, habilita ambas as famílias. Use a opção depois de configurar PTR, AAAA e autenticação para o IPv6 de saída. Essa preferência não altera IPv6 no sistema operacional.

## Certificados

O pacote inclui Lego para ACME DNS-01. A validação pode usar Cloudflare ou registros TXT publicados manualmente. Também é possível importar certificado/cadeia e chave PEM existentes. O certificado deve incluir `mail.<domínio>` e `webmail.<domínio>`.

A renovação é manual pela tela de certificados, com o servidor parado. Não há agendamento automático nesta versão. Certificados autoassinados servem apenas para testes locais.

## HTTPS compartilhado

No Windows, HTTP.sys permite compartilhar a 443 com outros hostnames, inclusive sites IIS. A primeira preparação e a troca do certificado precisam de administrador. Veja [WINDOWS-HTTPS.md](WINDOWS-HTTPS.md).

No Linux, marque **Usar ao lado do Apache**. O programa pede autenticação administrativa pelo polkit, instala um VirtualHost, testa a configuração e recarrega o Apache. O backend fica em `127.0.0.1:8787`, ou na porta interna escolhida. Apache 2.4, Python 3, polkit e OpenSSL precisam estar instalados. São suportados os layouts `/etc/apache2` e `/etc/httpd/conf`.

Sem essa opção, o CloudieMail serve HTTPS diretamente. Ao desativar a integração, escolha uma porta livre se o Apache continuar usando a 443.

## Clientes de email

Use `mail.<domínio>` para entrada e saída, o endereço completo como usuário e a senha da conta. Prefira IMAPS 993 e SMTPS 465 com autenticação. Os números devem corresponder à configuração do servidor.

O IMAP oferece pastas, anexos, busca, flags persistentes, APPEND/COPY/MOVE e notificações IDLE. Enviados, Rascunhos, Lixeira e Lixo eletrônico são identificados por SPECIAL-USE. O mantenedor confirmou o uso do servidor Windows com o Mail do iOS. Consulte [IMAP e antispam](IMAP-ANTISPAM.md) para capacidades, configuração e limites.

## Atualizações e backup

Encerre o servidor, inclusive na bandeja, e faça backup de `config/`, `data/` e de qualquer certificado referenciado fora dessas pastas. Preserve toda a pasta de dados: ela inclui o banco, as chaves DKIM e a chave que protege a fila de saída.

Extraia o pacote novo e transfira a configuração e os dados preservando os caminhos. Atualize executáveis e webmail juntos em mudanças de versão. Não inicialize outro banco nem recrie as contas existentes.

Bancos do esquema 1 são migrados para o esquema 2 automaticamente. Para voltar a um executável que só entende o esquema 1, restaure o backup anterior à migração. Para migrar entre Windows e Linux, use backup/restauração do Firebird e ajuste os caminhos.

## Filtro antispam

Em **Configurações → Preferências e limites**, ative o filtro, escolha se deseja verificar o DNS do remetente, ajuste a pontuação e informe emails ou domínios bloqueados, separados por espaço. O padrão é filtro ligado, DNS ligado e pontuação 5. As alterações entram em vigor ao reiniciar o servidor.

Mensagens classificadas como spam vão para **Lixo eletrônico** e continuam criptografadas. Para recuperar uma mensagem, mova-a para a Caixa de entrada no webmail ou no cliente IMAP. Essa ação não treina um modelo nem cria uma exceção automática.
