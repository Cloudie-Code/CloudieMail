# Validação da versão 0.1.0

Última revisão: 6 de novembro de 2026. Esta versão é distribuída como pré-release.

## Compilação e testes

| Verificação | Resultado |
| --- | --- |
| Linux GCC Release | Console e GTK4 compilados; runtime GCC/C++ estático |
| Windows MSVC x64 /MT | Console e GUI compilados pelo Wine |
| CTest Linux | Sete suítes passaram: configuração, criptografia/banco, pastas/fila, integração TLS, backend Apache, transporte SMTP e assistente |
| Protocolos e webmail | 36 verificações por transporte HTTP, com banco Firebird e conexões TLS reais |
| SMTP de saída | STARTTLS, envelope, dot-stuffing, rejeições 451/550, respostas multilinha, limites de texto, CA inválida e downgrade testados no Linux |
| IPv4/IPv6 | IPv4 por padrão e opt-in IPv6 testados com sockets locais; configurações antigas preservam o novo padrão |
| MSVC sob Wine | 30 verificações de criptografia, 11 de configuração, 12 do adaptador HTTP.sys e 2 de processos passaram |
| Navegador Chromium | Login, identidade do remetente, pastas, marcadores, estrelas, rascunhos, anexos, Unicode, cinco idiomas, fila e logout passaram |
| Interface GTK4 | Assistente completo, criação de conta, início/parada e preferências exercitados; screenshots geradas com dados de demonstração |
| Pacotes realocados | Lançadores, assistente, runtime Linux, GUI GTK4 e testes Windows compatíveis com Wine passaram em diretório temporário com espaços |
| MIME em JavaScript | Unicode, anexos, isolamento de HTML e limites do parser passaram |

Os testes usam contas temporárias e não enviam mensagens a destinatários reais. Uma entrega ao Gmail foi observada pelo operador em Windows, mas isso não representa uma matriz de interoperabilidade com provedores e clientes.

## Verificações anteriores mantidas como referência

- DKIM de mensagem normal e corpo vazio validado independentemente com dkimpy.
- Migração do esquema 1 para 2, preservando contas, senha, UID e conteúdo criptografado.
- VirtualHost Apache com TLS, outro site no mesmo servidor, rollback e remoção em instalação isolada.
- Seleção IPv4/IPv6 também exercitada no binário Windows sob Wine com STARTTLS.

## Limitações conhecidas

- IMAP parcial, limitado à INBOX. Ainda faltam funcionalidades usadas por Apple Mail, Outlook e Thunderbird.
- Sem verificação SPF/DKIM/DMARC de entrada, antispam ou DSN automático.
- Sem renovação agendada de certificados ou integração com serviço Windows/systemd.
- HTML de mensagens não é renderizado; conteúdo sem alternativa textual pode ser baixado como original.
- Limite atual de 1.000 mensagens por conta; busca e leitura podem carregar mensagens inteiras em memória.
- Faltam auditoria de segurança independente, fuzzing, testes prolongados de carga e validação completa de restauração.

## Windows nativo e Wine

O Wine não permite concluir toda a validação do Windows. O Firebird embedded encontra uma incompatibilidade de mapeamento de memória; testes de ACL não equivalem a permissões NTFS reais. O teste de recusa de certificado autoassinado sem CA explícita falhou no backend Schannel sob Wine. A confiança TLS, o listener HTTP.sys e a instalação completa precisam ser testados no Windows nativo. A verificação de certificados permanece habilitada no código.

## Memória

Há apontamentos do LeakSanitizer no runtime Firebird/iconv. O reprodutor `tests/firebird_runtime_probe.cpp` mostra o problema sem ligar o código do CloudieMail, inclusive após `fb_shutdown`. A origem precisa de investigação; isso não demonstra ausência de vazamentos no aplicativo.

Consulte [BUILDING.md](BUILDING.md) para repetir os testes e [ROADMAP.md](ROADMAP.md) para os próximos passos.
