# E-Book Content Licensing

> **Scope.** This is the clearance record for e-book content the reader is
> allowed to **bundle and publicly redistribute**. It lists the titles, the
> license basis, the sourcing policy, and the caveats to check before
> publishing. The actual `.epub` files are NOT checked into this tree (see
> [`VENDOR_BLOBS.md`](VENDOR_BLOBS.md) for why large third-party assets stay
> out of git); the working library is managed off-repo.

The reader renders EPUB (see [`EPUB_CONFORMANCE.md`](EPUB_CONFORMANCE.md)).
Any title shipped on-device or published as a bundle must be redistributable,
which means either public domain or a license that explicitly permits
redistribution. This document records what is cleared and why.

---

## Sourcing policy

1. **Primary source: Standard Ebooks.** Hand-typeset editions of
   public-domain works, each dedicated to the public domain under
   **CC0 1.0 Universal**. CC0 imposes no attribution or redistribution
   obligations, which makes these the cleanest sources to bundle and
   republish. Download URL pattern (compatible EPUB):
   `https://standardebooks.org/ebooks/<author>/<title>/downloads/<author>_<title>.epub?source=download`
2. **Public-domain basis of the underlying text: US copyright.** Works
   published 1929 or earlier are public domain in the US; as of 2026 the line
   has advanced through 1930. Standard Ebooks tracks US public domain.
3. **Policy: Standard-Ebooks-only.** If Standard Ebooks does not carry a clean
   edition of a title, the title is **not bundled** rather than substituted
   with a lower-quality edition. Project Gutenberg is a valid public-domain
   fallback (strip the Project Gutenberg trademark/boilerplate before
   redistribution), but is not used for the shipped set today.

## Caveats to check before publishing

- **Translations are separately copyrighted.** For works not originally in
  English the SE edition wraps a specific public-domain translation
  (for example Constance Garnett for the Russian novels, Isabel F. Hapgood
  for `Les Miserables`, John Ormsby for `Don Quixote`). These translations are
  US public domain; if publishing into life-plus-70 jurisdictions, verify the
  translator died at least 70 years ago.
- **Early-20th-century titles** (for example `Ulysses`, `The Great Gatsby`,
  `The Mysterious Affair at Styles`) are public domain in the US but cleared
  life-plus-70 only recently. Spot-check for non-US distribution.

---

## Cleared catalog (64 titles, Standard Ebooks CC0)

### Fiction -- classics
- Pride and Prejudice -- Jane Austen
- Frankenstein -- Mary Shelley
- Moby-Dick -- Herman Melville
- The Picture of Dorian Gray -- Oscar Wilde
- Great Expectations -- Charles Dickens
- A Tale of Two Cities -- Charles Dickens
- The Adventures of Huckleberry Finn -- Mark Twain
- Jane Eyre -- Charlotte Bronte
- Wuthering Heights -- Emily Bronte
- Heart of Darkness -- Joseph Conrad
- The Great Gatsby -- F. Scott Fitzgerald
- Ulysses -- James Joyce
- Dubliners -- James Joyce

### Sci-fi and adventure
- The War of the Worlds -- H. G. Wells
- The Time Machine -- H. G. Wells
- Journey to the Center of the Earth -- Jules Verne
- A Princess of Mars -- Edgar Rice Burroughs
- Tarzan of the Apes -- Edgar Rice Burroughs
- The Call of the Wild -- Jack London
- White Fang -- Jack London
- Treasure Island -- Robert Louis Stevenson
- King Solomon's Mines -- H. Rider Haggard
- Gulliver's Travels -- Jonathan Swift

### Horror and gothic
- Dracula -- Bram Stoker
- The Strange Case of Dr Jekyll and Mr Hyde -- Robert Louis Stevenson
- The Turn of the Screw -- Henry James
- The Castle of Otranto -- Horace Walpole

### Mystery and detective
- The Adventures of Sherlock Holmes -- Arthur Conan Doyle
- The Hound of the Baskervilles -- Arthur Conan Doyle
- The Moonstone -- Wilkie Collins
- The Woman in White -- Wilkie Collins
- The Innocence of Father Brown -- G. K. Chesterton
- The Mysterious Affair at Styles -- Agatha Christie

### Fantasy, fairy tales and children's
- Alice's Adventures in Wonderland -- Lewis Carroll
- The Wonderful Wizard of Oz -- L. Frank Baum
- The Jungle Book -- Rudyard Kipling
- The Wind in the Willows -- Kenneth Grahame
- The Secret Garden -- Frances Hodgson Burnett
- Anne of Green Gables -- L. M. Montgomery

### Poetry
- Leaves of Grass -- Walt Whitman
- Paradise Lost -- John Milton

### Drama and plays
- Hamlet -- William Shakespeare
- Macbeth -- William Shakespeare
- The Importance of Being Earnest -- Oscar Wilde
- Pygmalion -- George Bernard Shaw
- A Doll's House -- Henrik Ibsen

### World literature (public-domain translations)
- Crime and Punishment -- Fyodor Dostoevsky
- The Brothers Karamazov -- Fyodor Dostoevsky
- Anna Karenina -- Leo Tolstoy
- Les Miserables -- Victor Hugo
- The Count of Monte Cristo -- Alexandre Dumas
- The Three Musketeers -- Alexandre Dumas
- Madame Bovary -- Gustave Flaubert
- Don Quixote -- Miguel de Cervantes

### Non-fiction and philosophy
- Meditations -- Marcus Aurelius
- The Art of War -- Sun Tzu
- On the Origin of Species -- Charles Darwin
- The Prince -- Niccolo Machiavelli
- Tao Te Ching -- Laozi

### History, memoir and travel
- Narrative of the Life of Frederick Douglass -- Frederick Douglass
- The History of the Decline and Fall of the Roman Empire -- Edward Gibbon
- The Histories -- Herodotus
- Walden -- Henry David Thoreau
- The Voyage of the Beagle -- Charles Darwin
