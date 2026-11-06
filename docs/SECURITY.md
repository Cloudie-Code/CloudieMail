# Segurança e limites de confiança

Este documento descreve controles implementados e lacunas do protótipo. Não é uma auditoria nem uma garantia de resistência a todos os ataques.

## Chaves e senhas

- Chaves aleatórias por conta, nunca derivadas de domínio ou timestamp.
- Identificador `dominio/timestamp-UTC/id-aleatorio`; o ID evita colisões entre contas criadas no mesmo segundo.
- Salt independente para cada proteção de chave privada. Senhas e chaves privadas pessoais das contas não são persistidas em claro. Chaves TLS, DKIM e de proteção da fila são segredos do servidor, armazenados com permissões restritas.
- Argon2id13: 64 MiB e opslimit 3 da libsodium. Esses parâmetros não devem ser descritos como idênticos a todos os parâmetros da segunda configuração RFC 9106, que também especifica paralelismo próprio.
- A validação da senha ocorre pela abertura autenticada da chave privada. Não é necessário um verificador de senha separado.
- Chaves privadas de sessão usam limpeza explícita e tentativa de bloqueio de páginas via sodium_mlock. O sistema operacional pode negar o bloqueio. Outras cópias temporárias de senha e conteúdo em bibliotecas/strings não têm garantia de apagamento completo.
- Troca de senha depende da senha atual. O comando informa que é necessário reiniciar o servidor para invalidar sessões antigas. Revogação imediata distribuída ainda não está implementada.
- Não há fluxo de recuperação. Backups antigos mantêm a proteção pela senha antiga; trocar a senha não revoga cópias antigas do banco.

## Armazenamento e autenticidade

O Firebird armazena o endereço da conta, chave pública, metadados da chave, tamanho/data/flags, pastas e marcadores das mensagens e ciphertext. Conteúdo e cabeçalhos originais ficam dentro da mensagem criptografada. Não existe índice de assunto/remetente em claro.

Consultas usam parâmetros. Leituras, flags e exclusões incluem tanto o endereço autenticado quanto o UID. Uma entrega a vários destinatários locais é transacional. Há quota por conta e limite inicial de 1.000 mensagens por caixa.

Sealed boxes fornecem confidencialidade e integridade para o destinatário, **não assinatura do remetente**. Uma pessoa com acesso de escrita ao banco pode adulterar metadados, excluir/reordenar mensagens ou substituir chaves públicas de novas entregas. O modelo não protege contra um administrador/servidor ativamente comprometido. A entrada passa pelo filtro descrito em [IMAP-ANTISPAM.md](IMAP-ANTISPAM.md), com SPF, DKIM e avaliação conservadora de DMARC. Autenticação de domínio não garante que uma mensagem seja legítima.

Em Firebird embedded, a proteção do arquivo é a fronteira de acesso. O usuário lógico SYSDBA não substitui permissões do sistema operacional. Não abra a porta Firebird à Internet. O código cria diretórios novos restritos e arquivo 0600 no Linux; permissões de diretórios preexistentes precisam ser provisionadas corretamente. ACLs Windows ainda precisam de validação.

## Transporte e aplicação

- TLS mínimo 1.2, sem autenticação em conexão não criptografada; TLS implícito em HTTPS/IMAP/POP3/submissão.
- STARTTLS obrigatório no recebimento nesta versão. Isso pode rejeitar MTAs que não suportam TLS e não representa a política universal do SMTP na Internet.
- Submissão exige autenticação e remetente igual à conta. Relay para destinos externos exige uma conta autenticada e a opção de saída habilitada. O envelope e o cabeçalho From são vinculados à conta; clientes sem autenticação só podem entregar a contas locais. O CloudieMail entrega diretamente ao MX, sem credenciais de relay externo.
- IP e conta possuem limites de autenticação; há no máximo duas derivações Argon2id simultâneas. O limite por IP também conta autenticações bem-sucedidas para limitar consumo de CPU; precisa ser calibrado para NATs e uso real.
- Contas inexistentes passam por derivação de chave fictícia, reduzindo diferenças de tempo; não há alegação de indistinguibilidade formal.
- Conexões e linhas limitadas, timeout de handshake, deadline de sessão, limite de tamanho de mensagem e de destinatários.
- Cookie `__Host-cloudiemail`, Secure, HttpOnly, SameSite=Strict, Path=/; token aleatório, expiração absoluta, revogação no logout.
- Mutação HTTP verifica Origin exato e token CSRF. Host fica preso a `webmail.<dominio>` e porta configurada.
- CSP restritiva, sem scripts inline/CDN/iframes, nosniff, HSTS, no-store, Referrer-Policy e COOP.
- Emails entram na página via textContent. HTML de email não é executado; imagens externas e rastreadores não são carregados. Base64, quoted-printable, cabeçalhos RFC 2047, parâmetros de nome RFC 2231 e multipart são decodificados no navegador. HTML sem parte texto exige baixar o original; não há renderização HTML rica.
- Não há logs de senha, token, corpo ou assunto no caminho de atendimento dos protocolos. Erros de cliente são descartados nesta fase; o painel mantém até 500 eventos sem conteúdo sensível. Auditoria durável e abrangente permanece pendente.

## Fila de saída

A cópia em Enviados usa a chave pública pessoal da conta. A fila SMTP usa XChaCha20-Poly1305 com uma chave aleatória separada em `outbound-key.json`, permitindo entregar com o usuário desconectado. Remetente, destinatário e identificador entram nos dados autenticados. Um administrador com acesso a essa chave consegue abrir mensagens pendentes: a fila não tem a mesma fronteira de sigilo das caixas pessoais. Inclua essa chave nos backups da instalação.

Há limite de 100 destinatários pendentes por remetente, tentativas com espera crescente por até cinco dias e remoção do payload da fila após sucesso/falha terminal. Metadados de entrega são retidos por 30 dias. Não há DSN automático: falhas ficam visíveis em Entregas de saída. Uma interrupção após aceitação pelo MX e antes da confirmação local pode causar repetição, como em filas SMTP de entrega pelo menos uma vez.

Saída exige STARTTLS com verificação de certificado e TLS 1.2+. Não há fallback em claro. Destinos MX privados, loopback e as principais faixas reservadas são bloqueados no callback de abertura de socket. SPF/DKIM/DMARC recebidos não são validados; assinatura DKIM cobre apenas a saída.

## Limites de recursos

Defaults: 16 conexões, 10 MiB por email, 20 destinatários, 256 MiB e 1.000 mensagens por caixa, duas autenticações Argon2id simultâneas, 128 sessões web e duração máxima de sessão de 300 segundos. O consumo de memória real inclui Firebird, threads, buffers e até 128 MiB apenas para duas derivações concorrentes. A implementação ainda carrega uma mensagem inteira em memória; não foi feita validação de carga nem promessa de consumo mínimo.

## Antes de produção

Revisão independente, fuzzing dos parsers, testes de concorrência/DoS, validação completa dos protocolos, testes de restauração, política de recuperação de chaves, antispam, observabilidade, proteção de segredos e isolamento do processo. Use serviços com privilégio mínimo, limites de SO, proteção de swap/core dumps e ciclo de atualização de dependências. Nenhuma dessas etapas é substituída por passar na suíte inicial.

## Referências técnicas

- [Libsodium: password hashing](https://doc.libsodium.org/password_hashing/default_phf)
- [Libsodium: sealed boxes](https://libsodium.gitbook.io/doc/public-key_cryptography/sealed_boxes)
- [RFC 9106: Argon2](https://www.rfc-editor.org/rfc/rfc9106.html)
- [Firebird 5: instalação e acesso](https://www.firebirdsql.org/file/documentation/html/en/firebirddocs/qsg5/firebird-5-quickstartguide.html)
- [Cloudflare: email e DNS-only](https://developers.cloudflare.com/dns/troubleshooting/email-issues/)
- [Cloudflare: criação de registros](https://developers.cloudflare.com/api/resources/dns/subresources/records/methods/create/)
- [StatusNotifierItem](https://specifications.freedesktop.org/status-notifier-item/latest-single/)

## IMAP e antispam

APPEND criptografa o conteúdo antes da gravação. COPY/MOVE operam apenas dentro da conta autenticada; MOVE e EXPUNGE usam transações. Flags e inscrições ficam no Firebird. Literais, profundidade MIME, quantidade de partes, resposta FETCH e consultas DNS têm limites para controlar o consumo de recursos.

O antispam atua em entregas SMTP não autenticadas. Resultados recebidos em cabeçalhos `X-CloudieMail-*` reservados são removidos e substituídos pelo resultado local. Falhas de DNS não causam rejeição nem classificação isolada como spam. A classificação não descarta mensagens: a pasta Junk mantém a mesma criptografia das outras pastas. As consultas DNS por HTTPS enviam nomes de domínio à Cloudflare, sem senha, token ou conteúdo de mensagens.
