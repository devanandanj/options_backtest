from datetime import date
from jugaad_data.nse import stock_df

df = stock_df(
    symbol="IDFCFIRSTB",
    from_date=date(2022, 1, 1),
    to_date=date(2024, 7, 5),
    series="EQ"
)
df.to_csv("../data/sample/idfcfirstb_underlying_jan2022_july2024.csv", index=False)
print(df.columns.tolist())