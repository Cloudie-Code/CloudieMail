# IMAP e filtro de entrada

## IMAP

O listener IMAPS usa TLS desde o início da conexão. As contas usam o mesmo email e senha do webmail. A pasta pessoal tem prefixo vazio e separador `/`; nomes internacionais usam UTF-7 modificado conforme IMAP4rev1.

| Operação | Comportamento |
| --- | --- |
| LOGIN / AUTHENTICATE PLAIN | Autenticação dentro de TLS, com os limites de tentativas e Argon2id do servidor |
| LIST / LSUB / NAMESPACE | Pastas, hierarquia, inscrições e namespace pessoal |
| CREATE / DELETE / RENAME | Criação, exclusão e renomeação de pastas; renomeação da INBOX move suas mensagens e preserva a INBOX |
| SUBSCRIBE / UNSUBSCRIBE | Inscrições persistentes por conta |
| SELECT / EXAMINE / STATUS | Seleção com ou sem escrita, contagem, UIDNEXT, UIDVALIDITY, não lidas e recentes |
| APPEND | Literais com tamanho limitado; flags e data opcional; armazenamento criptografado e APPENDUID |
| FETCH | Flags, UID, tamanho, data interna, ENVELOPE, BODY/BODYSTRUCTURE, RFC822, seções MIME, cabeçalhos filtrados e trechos de anexos |
| STORE | Seen, Answered, Flagged, Deleted, Draft e palavras-chave; operações de adicionar/remover/substituir e SILENT |
| SEARCH | Grupos, AND implícito, OR/NOT, flags, palavras-chave, UID/sequência, tamanho, datas, cabeçalhos, texto e corpo MIME decodificado |
| COPY / MOVE | Operação transacional, preservando conteúdo, flags e data; novos UIDs no destino |
| EXPUNGE / UID EXPUNGE / CLOSE | Remoção das mensagens marcadas; CLOSE não envia respostas EXPUNGE |
| UNSELECT | Fecha a seleção sem excluir mensagens |
| IDLE | Notifica mensagens novas, alterações de flags e exclusões; DONE encerra a espera |
| ID / NOOP / CHECK / CAPABILITY / LOGOUT | Identificação e controle de sessão |

Enviados (`Sent`), Rascunhos (`Drafts`), Lixeira (`Trash`) e Lixo eletrônico (`Junk`) têm atributos SPECIAL-USE. As capacidades anunciadas são `IMAP4rev1 AUTH=PLAIN IDLE UIDPLUS MOVE UNSELECT NAMESPACE ID SPECIAL-USE`.

As flags ficam no Firebird e são compartilhadas pelo webmail e pelas sessões IMAP. Mover uma mensagem pelo webmail também atribui um UID novo no destino; isso evita reutilizar um UID já observado por um cliente. Mensagens existentes preservam seus UIDs na atualização. A sessão IDLE tem limite de 29 minutos e deve ser renovada pelo cliente.

O Mail do iOS é utilizado pelo mantenedor com o servidor Windows. A suíte de integração exercita o protocolo diretamente, incluindo concorrência e reinício. IMAP4rev2 e extensões não anunciadas, como CONDSTORE, QRESYNC, ACL e BINARY, não fazem parte desta versão.

## Antispam

O filtro vem ativado. Em **Preferências e limites**, é possível desligá-lo, desligar consultas DNS, mudar a pontuação de classificação e informar emails ou domínios bloqueados. A pontuação padrão é 5.

| Regra | Pontos |
| --- | ---: |
| GTUBE, marcador público de teste de antispam | 100 |
| Email ou domínio na lista de bloqueio | 100 |
| Anexo com extensão executável | 6 |
| Formulário HTML que pede senha | 6 |
| From duplicado ou MIME acima dos limites estruturais | 6 |
| Scripts, iframe ou URL javascript em HTML | 5 |
| Falha DMARC estrita com política quarantine/reject | 5 |
| From inválido ou possível falsificação do domínio local | 3 |
| SPF fail sem DKIM alinhado válido | 2 |
| DKIM inválido, Date ausente ou Message-ID ausente | 1 por regra |

Mensagens que atingem a pontuação vão para **Lixo eletrônico**, sem descarte automático. Os cabeçalhos criptografados `X-CloudieMail-Spam`, `X-CloudieMail-Spam-Score`, `X-CloudieMail-Spam-Rules` e `X-CloudieMail-Authentication` registram o resultado. O remetente não pode fornecer esses resultados: o servidor substitui os cabeçalhos reservados recebidos pela rede. A submissão autenticada pelas contas locais não passa pelo filtro de entrada.

O filtro de conteúdo identifica padrões específicos; não substitui um antivírus e não garante detectar toda mensagem maliciosa. Mover uma mensagem de Junk para INBOX recupera o email, mas não treina um classificador.

## Autenticação do remetente e DNS

As consultas usam DNS por HTTPS da Cloudflare, sem token de API. Apenas nomes de domínio e tipos de registro são enviados; o conteúdo do email permanece no servidor. Há orçamento de seis segundos por mensagem, limite de consultas e cache dentro de cada avaliação. Erros e indisponibilidade de DNS ficam como `temperror` e não geram pontuação por si sós.

- **SPF:** IPv4/IPv6, ip4/ip6, a, mx, include, exists, redirect e qualificadores, com limite de dez termos que exigem consulta. Políticas com macros ou PTR ficam neutras; não são tratadas como uma prova de falha ou sucesso.
- **DKIM:** RSA-SHA256 e Ed25519-SHA256, canonicalização simple/relaxed, hash do corpo, cabeçalhos assinados, expiração e chave revogada. Assinaturas que cobrem apenas parte do corpo não autenticam o restante da mensagem. São avaliadas até quatro assinaturas por mensagem.
- **DMARC:** reconhece alinhamento exato comprovado por SPF ou DKIM. Aplica falha somente quando o domínio publica alinhamento estrito para ambos e uma política aplicável. Políticas que dependem de alinhamento relaxado, domínio organizacional ou descoberta em domínio pai ficam sem decisão de falha nesta versão.

## Atualizar uma instalação

Pare o servidor e faça backup de `config/`, `data/` e certificados externos. Substitua os arquivos do programa pelo pacote novo, mantendo os dados da instalação. O Firebird migra automaticamente para o esquema 4, sem recriar contas ou recriptografar mensagens. A migração pode ser retomada após interrupção entre as etapas de metadados e dados.

Depois da migração, o executável antigo não abre esse banco. Para voltar à versão anterior, restaure também o backup anterior do banco. O filtro é ativado por padrão ao ler uma configuração antiga; revise suas preferências antes de iniciar o servidor.

## Referências

- [IMAP4rev1 — RFC 3501](https://www.rfc-editor.org/rfc/rfc3501)
- [IDLE — RFC 2177](https://www.rfc-editor.org/rfc/rfc2177)
- [MOVE — RFC 6851](https://www.rfc-editor.org/rfc/rfc6851)
- [SPECIAL-USE — RFC 6154](https://www.rfc-editor.org/rfc/rfc6154)
- [SPF — RFC 7208](https://www.rfc-editor.org/rfc/rfc7208)
- [DKIM — RFC 6376](https://www.rfc-editor.org/rfc/rfc6376)
