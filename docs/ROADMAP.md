# Próximas versões

A versão 0.2.0 acrescenta pastas e operações IMAP, sincronização IDLE e filtro antispam ao servidor, às interfaces e ao webmail. O foco das próximas versões é interoperabilidade, operação contínua e segurança.

## Protocolos e entrega

- Ampliar as extensões IMAP com CONDSTORE/QRESYNC e UTF8=ACCEPT.
- Ampliar os cenários do Mail do iOS já utilizado pelo mantenedor; testar também Thunderbird e Outlook.
- Completar o lock de caixa POP3 e o comportamento de UPDATE após falhas.
- Implementar DSN/bounces automáticos e ampliar testes de entrega externa.
- Ampliar o antispam com SPF macros/PTR, alinhamento DMARC relaxado, reputação e aprendizado opcional.
- Diagnosticar PTR, propagação DNS e conectividade SMTP pelo programa.

## Operação

- Agendar renovação de certificados e recarregar TLS sem encerrar sessões.
- Adicionar integração com systemd e serviço Windows, com privilégios mínimos.
- Testar atualização, restauração e reinício após falhas em Windows nativo.
- Homologar bandeja em GNOME, KDE e Windows, inclusive reinício do shell.
- Ampliar testes de quota, concorrência, carga e consumo de memória.

## Webmail e contas

- Oferecer visualização HTML sanitizada e isolada.
- Definir recuperação de chaves e recuperação de conta sem comprometer o histórico criptografado.
- Revogar imediatamente todas as sessões ao trocar a senha.
- Rever o limite de 1.000 mensagens por conta e a busca em memória.

## Qualidade

- Investigar os apontamentos do LeakSanitizer no Firebird embedded.
- Homologar confiança TLS/Schannel, ACLs e HTTP.sys em Windows nativo.
- Adicionar fuzzing dos parsers e auditoria de segurança independente.
- Ampliar builds reprodutíveis e a matriz de distribuições Linux suportadas.
