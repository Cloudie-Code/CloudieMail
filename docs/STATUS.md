# Validação da versão 0.2.0

Última revisão: 6 de novembro de 2026. Esta versão é distribuída como pré-release.

## Compilação e testes

| Verificação | Resultado |
| --- | --- |
| Linux GCC Release | Console e GTK4 compilados; runtime GCC/C++ estático |
| Windows MSVC x64 /MT | Console e GUI compilados pelo Wine |
| CTest Linux | Nove suítes: IMAP/antispam, MIME/autenticação do remetente, configuração, criptografia/banco, pastas/fila, integração TLS, backend Apache, transporte SMTP e assistente |
| Protocolos e webmail | 36 verificações por transporte HTTP, com banco Firebird e conexões TLS reais |
| SMTP de saída | STARTTLS, envelope, dot-stuffing, rejeições 451/550, respostas multilinha, limites de texto, CA inválida e downgrade testados no Linux |
| IPv4/IPv6 | IPv4 por padrão e opt-in IPv6 testados com sockets locais; configurações antigas preservam o novo padrão |
| MSVC sob Wine | 30 verificações de criptografia, 14 de configuração, 41 de MIME/antispam, 12 do adaptador HTTP.sys e 2 de processos |
| Navegador Chromium | Login, identidade do remetente, pastas, marcadores, estrelas, rascunhos, anexos, Unicode, cinco idiomas, fila e logout passaram |
| Interface GTK4 | Assistente completo, criação de conta, início/parada e preferências exercitados; screenshots geradas com dados de demonstração |
| Pacotes realocados | Lançadores, assistente, runtime Linux, GUI GTK4 e testes Windows compatíveis com Wine passaram em diretório temporário com espaços |
| MIME em JavaScript | Unicode, anexos, isolamento de HTML e limites do parser passaram |

Os testes automatizados usam contas temporárias e não enviam mensagens a destinatários reais. O mantenedor confirmou o uso do CloudieMail em Windows com o Mail do iOS e entrega ao Gmail. Esse uso real está registrado separadamente das suítes automatizadas.

A integração IMAP usa sockets TLS e Firebird reais: pastas Unicode e hierarquia, assinaturas de pastas, APPEND, COPY/MOVE, UIDPLUS, flags entre sessões, EXAMINE, exclusões, MIME multipart e message/rfc822, trechos de anexos, filtros de cabeçalho, busca, IDLE, SASL PLAIN, reinício e isolamento entre contas. A mesma suíte verifica quarentena de GTUBE, remoção de cabeçalhos antispam forjados e criptografia no arquivo do banco.

A suíte MIME/antispam inclui SPF IPv4/IPv6, include/MX/redirect, limites de recursão, DKIM adulterado/revogado, ausência de DNS e regras de conteúdo. Os vetores DKIM simple/relaxed são gerados e conferidos independentemente com dkimpy; apenas mensagens de teste e chaves públicas entram no repositório.

## Verificações anteriores mantidas como referência

- DKIM de mensagem normal e corpo vazio validado independentemente com dkimpy.
- Migração do banco da versão 0.1.0 para o esquema 4, preservando contas, senha, UID e conteúdo criptografado.
- VirtualHost Apache com TLS, outro site no mesmo servidor, rollback e remoção em instalação isolada.
- Seleção IPv4/IPv6 também exercitada no binário Windows sob Wine com STARTTLS.

## Limitações conhecidas

- A implementação anuncia IMAP4rev1 e as extensões documentadas em [IMAP-ANTISPAM.md](IMAP-ANTISPAM.md). Não anuncia IMAP4rev2, CONDSTORE, QRESYNC ou BINARY.
- O antispam é baseado em regras e autenticação do remetente; não inclui antivírus, reputação externa ou aprendizado Bayesiano. SPF com macros/PTR e alinhamento DMARC relaxado permanecem sem avaliação completa; veja [os limites do filtro](IMAP-ANTISPAM.md).
- Sem DSN automático.
- Sem renovação agendada de certificados ou integração com serviço Windows/systemd.
- HTML de mensagens não é renderizado; conteúdo sem alternativa textual pode ser baixado como original.
- Limite atual de 1.000 mensagens por conta; busca e leitura podem carregar mensagens inteiras em memória.
- Faltam auditoria de segurança independente, fuzzing, testes prolongados de carga e validação completa de restauração.

## Windows nativo e Wine

O Wine não permite concluir toda a validação do Windows. O Firebird embedded encontra uma incompatibilidade de mapeamento de memória; testes de ACL não equivalem a permissões NTFS reais. O teste de recusa de certificado autoassinado sem CA explícita falhou no backend Schannel sob Wine. O funcionamento no Windows nativo foi confirmado pelo mantenedor; a matriz automatizada de casos de falha de confiança TLS, ACLs e HTTP.sys ainda precisa ser executada nesse sistema. A verificação de certificados permanece habilitada no código.

## Memória

Há apontamentos do LeakSanitizer no runtime Firebird/iconv. O reprodutor `tests/firebird_runtime_probe.cpp` mostra o problema sem ligar o código do CloudieMail, inclusive após `fb_shutdown`. A origem precisa de investigação; isso não demonstra ausência de vazamentos no aplicativo.

Consulte [BUILDING.md](BUILDING.md) para repetir os testes e [ROADMAP.md](ROADMAP.md) para os próximos passos.
