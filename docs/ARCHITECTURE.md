# Organização do projeto

```text
cmake/                 Dependências e alvos de teste do CMake
config/                Configuração de exemplo
include/cloudiemail/   Interfaces compartilhadas do servidor
src/
  app/                 Console, assistente, configuração inicial e traduções
  core/                Criptografia, configuração persistente e Firebird
  mail/                SMTP, IMAP, POP3, MIME, DKIM e fila de saída
  net/                 Listeners, HTTP e cliente DNS/Cloudflare
  platform/            Execução de processos
    linux/             GTK4 e integração Apache
    windows/           Win32 e HTTP.sys
web/                   Webmail e catálogo de traduções
scripts/               Preparação, compilação e empacotamento
  runtime/             Ferramentas instaladas junto ao programa
packaging/             Lançadores Linux/Windows e instruções do pacote
tests/                 Testes C++, integração de rede e navegador
docs/                  Guias e capturas de tela
build/                 Builds locais, fora do Git
Output/                Pacotes e arquivos de release, fora do Git
```

`cloudiemail_core` contém as regras compartilhadas pelas interfaces gráfica e de terminal. O assistente é o mesmo no GTK4, Win32 e console. As integrações com HTTP.sys e Apache adaptam o transporte do webmail ao sistema operacional; os handlers HTTP continuam compartilhados.

Os cabeçalhos em `include/cloudiemail/` são usados por vários módulos. Detalhes exclusivos de uma plataforma, como a adaptação de requisições HTTP.sys, ficam junto à implementação.

Os testes criam bancos, certificados e contas temporários. Dados de instalação, certificados pessoais, dependências baixadas e arquivos de build não entram no repositório. Scripts de suporte específicos de uma instalação ficam fora da distribuição.
