#!/usr/bin/env python3
"""Checks that the docs stay separated: reference in the present tense, TODO with open items only, the diary in git.

Three kinds of text (full rule in CLAUDE.md, "Documentation"):
- Reference (README.md, CLAUDE.md, docs/*.md, docs/passos/*.md): how things are today. No dates, no "it used to be", no
  "done/fixed in", no test reports ("verified"). The history goes in the commit message.
- TODO.md: only what is open. A finished item leaves the file (no "done", no strikethrough).
- Diary: commit messages and docs/investigacoes/<date>/ (not checked here).

Dates inside a path in backticks (`docs/screenshots/2026-10-22/...`, `<commit>:docs/investigacoes/...`) are allowed.

Usage:
  python tools/doclint.py              only the lines the commit adds (git diff --cached) + the whole TODO.md
  python tools/doclint.py --all        everything, with a count per file (what is still left to migrate)
  python tools/doclint.py <file...>    those files, whole
Exits with 1 if it finds any problem.
"""
import re
import subprocess
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
TODO = "TODO.md"

DATA = re.compile(r"\b20\d\d-\d\d-\d\d\b")
CODIGO = re.compile(r"`[^`]*`")  # backticks in pairs, left to right


def tira_caminhos(linha):
    """Blanks the backtick spans that are paths (contain "/"): a date inside a path is allowed."""
    return CODIGO.sub(lambda m: "``" if "/" in m.group(0) else m.group(0), linha)


# Each rule has an English half (our docs) and a Portuguese half (text that arrives in upstream merges). The English half is
# narrower than a word-for-word copy: "fixed in" and "used to" are everyday present-tense English ("fixed in place", "the button
# used to exit"), so a fix only counts as history when it names a commit, and "used to" only after a pronoun ("it used to").
FEITO_EM_COMMIT = r"(done|fixed|resolved) (in|on) (commit\b|[0-9a-f]{7,}\b)"
HISTORIA = re.compile(
    r"\b(feit[oa]s? em|corrigid[oa]s? (em|no|na|de passagem)|antes (era|ficava|saía|vinha|dava|zerava|ia|não)|"
    r"na master|(depois|antes|desde) d[oe] merge\b|conferid[oa]s?\b|relato de|"
    r"fixed (in passing|along the way)|" + FEITO_EM_COMMIT + r"|(it|this|that|they|what|which) used to\b|on master\b|"
    r"(verified|checked) by the owner\b|"
    r"report (by|from|of) the owner\b)",
    re.IGNORECASE,
)
CITACAO = re.compile(r'"[^"\n]*"')  # a word in double quotes is quoted (e.g. the rule itself), not history
FEITO = re.compile(r"(\bfeit[oa]s? (em|no|na|pel[oa]s?)\b|\*\*feit|— feit|\(feit|\bna master\b|~~|\bcorrigid[oa]s? em\b|"
                   r"\bconferid[oa]s? pelo dono\b|\b" + FEITO_EM_COMMIT + r"|\*\*(done|fixed)\b|— (done|fixed)\b|"
                   r"\((done|fixed)\b|\bon master\b|\b(verified|checked) by the owner\b)", re.IGNORECASE)
MAX_LINHAS_ITEM = 6  # a TODO item (bullet or number) with more lines than this has turned into a report


def e_referencia(rel):
    rel = rel.replace("\\", "/")
    if not rel.endswith(".md") or rel == TODO:
        return False
    if rel in ("README.md", "CLAUDE.md", "CONTRIBUTING.md"):
        return True
    return re.fullmatch(r"docs/(passos/)?[^/]+\.md", rel) is not None


def problemas_referencia(linha):
    """[(key, message)]; the key is the text found, to compare against the line the commit removed."""
    sem_caminhos = tira_caminhos(linha)
    achados = []
    for d in DATA.findall(sem_caminhos):
        achados.append((d, f"date ({d})"))
    for m in HISTORIA.finditer(CITACAO.sub('""', sem_caminhos)):
        achados.append((m.group(0).lower(), f'history ("{m.group(0)}")'))
    return achados


def checa_todo(texto):
    erros = []
    linhas = texto.split("\n")
    inicio, n = None, 0
    for i, linha in enumerate(linhas, 1):
        sem_caminhos = tira_caminhos(linha)
        if DATA.search(sem_caminhos):
            erros.append((i, "date in the TODO (the date belongs to the commit)"))
        m = FEITO.search(sem_caminhos)
        if m:
            erros.append((i, f'finished item in the TODO ("{m.group(0)}"): delete the line and document it in the reference doc'))
        if re.match(r"\s*(- |\d+\. )", linha) and not linha.startswith("  " * 2):
            if inicio and n > MAX_LINHAS_ITEM:
                erros.append((inicio, f"item with {n} lines (max {MAX_LINHAS_ITEM}): the detail goes in the reference doc"))
            inicio, n = i, 1
        elif inicio and linha.startswith("  ") and linha.strip():
            n += 1
        else:
            if inicio and n > MAX_LINHAS_ITEM:
                erros.append((inicio, f"item with {n} lines (max {MAX_LINHAS_ITEM}): the detail goes in the reference doc"))
            inicio, n = None, 0
    if inicio and n > MAX_LINHAS_ITEM:  # last item of the file
        erros.append((inicio, f"item with {n} lines (max {MAX_LINHAS_ITEM}): the detail goes in the reference doc"))
    return erros


def linhas_acrescentadas():
    """({file: [(num, line)]} of the lines the commit adds, {file: text of the lines it removes})."""
    saida = subprocess.run(["git", "diff", "--cached", "-U0", "--no-color", "--", "*.md"], cwd=RAIZ,
                           capture_output=True).stdout.decode("utf-8", "replace")
    por_arquivo, tiradas, atual, num = {}, {}, None, 0
    for linha in saida.split("\n"):
        if linha.startswith("+++ "):
            atual = linha[6:] if linha.startswith("+++ b/") else None
        elif linha.startswith("--- "):
            continue
        elif linha.startswith("@@"):
            num = int(re.search(r"\+(\d+)", linha).group(1))
        elif atual and linha.startswith("+"):
            por_arquivo.setdefault(atual, []).append((num, linha[1:]))
            num += 1
        elif atual and linha.startswith("-"):
            tiradas.setdefault(atual, []).append(linha[1:])
    return por_arquivo, tiradas


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    args = sys.argv[1:]
    erros = []
    tiradas = {}
    if args == ["--all"]:
        arquivos = [p.relative_to(RAIZ).as_posix() for p in
                    [RAIZ / "README.md", RAIZ / "CLAUDE.md", RAIZ / "CONTRIBUTING.md", *sorted((RAIZ / "docs").glob("*.md")),
                     *sorted((RAIZ / "docs/passos").glob("*.md"))] if p.exists()]
        alvo = {a: list(enumerate((RAIZ / a).read_text("utf-8").split("\n"), 1)) for a in arquivos}
    elif args:
        alvo = {Path(a).resolve().relative_to(RAIZ).as_posix(): None for a in args}
        for a in alvo:
            alvo[a] = list(enumerate((RAIZ / a).read_text("utf-8").split("\n"), 1))
    else:
        alvo, tiradas = linhas_acrescentadas()
    for rel, linhas in alvo.items():
        if not e_referencia(rel):
            continue
        # An old line that was only edited: what was already in the same file's removed lines doesn't count (docs migrate gradually).
        ja_tinha = {}
        for velha in tiradas.get(rel, []):
            for chave, _ in problemas_referencia(velha):
                ja_tinha[chave] = ja_tinha.get(chave, 0) + 1
        for num, linha in linhas:
            for chave, p in problemas_referencia(linha):
                if ja_tinha.get(chave, 0) > 0:
                    ja_tinha[chave] -= 1
                    continue
                erros.append((rel, num, p))
    todo = RAIZ / TODO
    if todo.exists() and (not args or args == ["--all"] or TODO in alvo):
        erros += [(TODO, n, p) for n, p in checa_todo(todo.read_text("utf-8"))]

    if args == ["--all"]:
        contagem = {}
        for rel, _, _ in erros:
            contagem[rel] = contagem.get(rel, 0) + 1
        for rel in sorted(contagem, key=lambda r: -contagem[r]):
            print(f"{contagem[rel]:5}  {rel}")
        print(f"{len(erros)} problem(s) in {len(contagem)} file(s)" if erros else "all clean")
    else:
        for rel, num, p in erros:
            print(f"{rel}:{num}: {p}")
        if erros:
            print(f"\n{len(erros)} problem(s). Reference stays in the present tense (history goes in the commit message); "
                  "TODO.md only holds what is open. Rule: CLAUDE.md, \"Documentation\".")
    return 1 if erros else 0


if __name__ == "__main__":
    sys.exit(main())
