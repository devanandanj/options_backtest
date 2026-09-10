from datetime import date
from jugaad_data.nse import derivatives_df

df = derivatives_df(
    symbol="IDFCFIRSTB",
    from_date=date(2024, 1, 1),
    to_date=date(2024, 1, 31),
    expiry_date=date(2024, 1, 25),   # pick an actual expiry Thursday in that month
    instrument_type="OPTSTK",
    option_type="CE",                 # "PE" for puts
    strike_price=80                   # pick a strike near spot for that period
)
df.to_csv("../data/sample/idfcfirstb_jan2024_ce_80.csv", index=False)